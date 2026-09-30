# duckdb-salesforce v0.18.0

> **Own-repo release.** Aggregates the 2026-09-29/30 development cycles on top
> of `v0.17.1`: **child-direction recursion** in
> `salesforce_relationship_graph` (P2.3), **Report Bridge base-object
> resolution from `reportTypeMetadata`** (P2.2 cut 2), the aggregate-pushdown
> pass compiling on both DuckDB 1.5.x and 2.0, the ARCHITECTURE.md factual
> corrections, and the **live-org evidence closing P1.4** (transparent
> SUM/AVG pushdown rejected). Also records the community update to v0.15.1
> (#2850 merged). Minor bump (new features).

## What changed

**`salesforce_relationship_graph` — child-direction recursion (P2.3, #77)**

`direction := 'child'` / `'both'` now walks resolved (named + queryable)
children depth-first up to `max_depth` (same clamp `[1,4]`, same parameter
bounding both directions), with ancestor-chain cycle protection
(`status='cyclic'`) and dotted relationship-name paths (`Contacts.Orders`).
Unnamed and not-queryable children are deliberate dead ends. At the default
`max_depth = 1` the output is byte-identical to the historical single-level
listing. Existing tests were updated from the old "children ignore max_depth"
contract; a dedicated guard file covers depth counts, cycles, path chains and
pruning.

**Report Bridge — base object from `reportTypeMetadata` (P2.2 cut 2, #78)**

The report `/describe`'s `reportTypeMetadata.categories[0].columns` carry
`fullyQualifiedName` values whose first segment is the base object
(`Case.CaseNumber` → `Case`). When the non-empty prefixes of the first
category collapse to exactly one object, that object becomes a base-object
candidate with provenance `report_type_metadata`, ranked after
`CustomEntity$` suffix resolution and before the static builtin map (official
per-report ground truth beats a static contract). Live-proven on a real org
(2026-09-30): `CaseList → Case`, `Opportunity → Opportunity`, and the joined
`OpportunityLead` type correctly does NOT collapse — the ambiguity guard is
what makes the source safe. When a report is blocked early by shape,
`base_object` still carries the documented best-effort ingredient (the raw
report type) with `base_object_resolved_by = 'unresolved'` — now pinned by
test. The builtin map is intentionally untouched (cut 2 covers the standard
types; the map remains the contract for describes without
`reportTypeMetadata`).

**DuckDB 2.0 readiness (chore, #76)**

`salesforce_agg_optimizer.cpp` — the newest, highest-risk file — now compiles
on **both** DuckDB 1.5.x and 2.0 with compile-time member detection (SFINAE +
`if constexpr`, no version macros), per the v2.0 canary evidence. Behavior on
the supported matrix is unchanged. The remaining 2.0 surface (~6 files) is
mapped for the dedicated migration cycle when 2.0 approaches GA; the
informational canary against `v2.0-cyanoptera` continues on demand.

**Docs (P2.5, #75; P1.4, #79)**

- `docs/ARCHITECTURE.md` main body: factual corrections — transport
  crossover is the shipped `sf_auto_bulk_threshold` (default 50,000, not the
  research-era 10,000), PK chunking described as shipped (`sf_bulk_chunks`,
  1–8 disjoint Id ranges), GraphQL marked as a never-implemented early-plan
  sketch, planned source tree marked as plan.
- **P1.4 closed by live evidence** (`docs/smoke/aggregate-precision-v0.17.1.md`):
  on a 3,144,206-row window of a real org, Salesforce's server-side SUM of a
  currency field under-reports the exact DuckDB DECIMAL result by ~1,160
  (floating-point summation); AVG diverges identically; MIN/MAX/COUNT match
  exactly. **Transparent SUM/AVG pushdown is rejected** (it would silently
  change results) — `salesforce_aggregate()` remains the explicit opt-in;
  MIN/MAX (v0.17.0) confirmed safe. Reproducible smoke:
  `scripts/run_smoke_agg_precision.ps1`; Report Bridge verification smoke:
  `scripts/run_smoke_report_soql_verify.ps1`.

**Community** — the v0.15.1 update merged upstream
([duckdb/community-extensions#2850](https://github.com/duckdb/community-extensions/pull/2850),
2026-09-30); the live descriptor mirror moved to v0.15.1. Updates past
v0.15.1 (v0.16.0–v0.18.0) are **not submitted** — any community action waits
for the maintainer's explicit OK.

## Why v0.18.0 (minor)

New user-visible features (relationship-graph recursion, Report Bridge
base-object resolution), same loose policy as v0.9–v0.17.

## Compatibility

Supported DuckDB line unchanged: **v1.5.4/v1.5.5/v1.5.6** (official since
v0.17.1). The v2.0 dual-compile is informational only — no v2 support claim.

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; post-merge offline suite **52 files, 1556 assertions, 0 failures**.
- `clang-format` (project pin 11.0.1) clean over all touched files.
- Per-branch official CI runs, all **9/9 green** (v1.5.4/v1.5.5/v1.5.6 ×
  linux/windows/osx): #76 → 36713650654, #77 → 36722332866, #78 →
  36732763657. Post-merge `main` run recorded below.

### Official CI run record

`MainDistributionPipeline.yml` run
[36743994783](https://github.com/flozer/duckdb-salesforce/actions/runs/36743994783)
(2026-09-30, `workflow_dispatch` against post-merge `main` @ `539b204`;
release-prep commit `a521d24` docs-only) — **9/9 green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |
| v1.5.6 | Pass | Pass | Pass |

## Gates

- Community update: **not submitted and not scheduled by this release** — the
  descriptor draft for this version is prepared and waits for the
  maintainer's explicit OK (single update from v0.15.1 straight to v0.18.0).

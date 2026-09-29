# duckdb-salesforce v0.16.0

> **Own-repo release.** Transparent `COUNT(field)` pushdown via a DuckDB
> `OptimizerExtension` (roadmap P1.2a) — the first plan-rewrite capability of
> the extension, implemented per the feasibility spec
> [`2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md`](superpowers/specs/2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md)
> and released with maintainer GO (issue #66 follow-up work had already
> shipped as v0.15.1 earlier the same day). Kill-switch:
> `SET sf_aggregate_pushdown = false`.

## What changed

**New: transparent `COUNT(field)` pushdown** (`src/salesforce_agg_optimizer.cpp`,
registered in `src/salesforce_extension.cpp`)

- A post-optimizer pass (`OptimizerExtension::optimize_function`, running
  after every built-in pass) rewrites `Aggregate(no groups: COUNT(col), ...)`
  sitting **directly** over a `salesforce_scan` Get into **one** server-side
  SOQL query — `SELECT COUNT(col) a0 FROM obj WHERE <pushed_where>` — whose
  single result row replaces the removed aggregate. The Aggregate node becomes
  a binding-preserving `LogicalProjection` reusing the aggregate's table
  index, so parent operators (LIMIT/ORDER BY/projection) need no changes.
- **Safety contract** — the rewrite fires only when every guard holds; any
  miss leaves the plan untouched (the always-correct row-scan + local
  aggregation fallback):
  - direct `LogicalGet` child only (an intervening residual `Filter` blocks
    it — the crash this guard prevents was caught by the new test during
    development);
  - no GROUP BY / grouping sets (local grouping stays correct and deferred);
  - every term is `COUNT` with exactly one column-ref child — no
    `DISTINCT`/`FILTER`/`ORDER BY`/expression children; `COUNT(*)` keeps the
    existing zero-column `SELECT COUNT()` path;
  - the referenced field is top-level, non-relationship, non-blob, resolved
    through the post-pruning `column_ids` binding space;
  - **zero residual filters** and empty standard `table_filters` (a
    pre-aggregated row must never be re-filtered locally);
  - kill-switch `sf_aggregate_pushdown` (BOOLEAN, default `true`).
- **Failure stance:** at scan time a transport or result-shape failure of the
  aggregate query is a **hard error** (correctness now depends on the server
  answer — unlike the `COUNT(*)` estimator there is no silent fallback). The
  error message points at the kill-switch.
- **Diagnostics:** `salesforce_last_soql()` shows the aggregate SOQL actually
  sent; `salesforce_query_cost()` flags `count_pushdown` with `rows_emitted=1`
  and `pages_fetched=0`; `salesforce_query_explain()` gains `role='aggregate'`
  rows (reason `aggregate_pushdown`).
- **Docs:** new `sf_aggregate_pushdown` setting section + Pushdown-reference
  note in both manuals (`docs/en/function_manual.md`, `docs/pt/function_manual.md`,
  parity kept).

**Also in this release (already validated in v0.15.1, now folded into the
supported-pin hygiene):** the `duckdb` submodule pin moved to tag **v1.5.6**
(`069cc9f9`, replacing the wrong-target dependabot proposal #69) and
`extension-ci-tools` to `8d2a39a` **aligned** with the workflow's reusable
ref + `ci_tools_version` (PR #71, superseding dependabot #70). The official
supported matrix is unchanged: **DuckDB v1.5.4 and v1.5.5**; v1.5.6 is
locally validated but not yet officially declared.

## Why v0.16.0 (minor)

New user-visible planner behavior behind a default-on setting — same loose
versioning policy as v0.9/v0.10/v0.11 (new feature = minor). No supported
DuckDB version was dropped.

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; **offline mock suite 51 files, 1490 assertions, 0 failures**,
  including the new guard test.
- `clang-format` (project pin 11.0.1) clean over all touched files.
- Local `scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4`:
  see the run record below.
- Official `MainDistributionPipeline.yml` on `main` post-merge: run
  [36611832353](https://github.com/flozer/duckdb-salesforce/actions/runs/36611832353)
  (2026-09-29). The feature branch's own run
  [36604401384](https://github.com/flozer/duckdb-salesforce/actions/runs/36604401384)
  was 6/6 green pre-merge.

### Local matrix run record

`scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4
-VcpkgToolchain C:/Users/fernando.souza/vcpkg/scripts/buildsystems/vcpkg.cmake`
(2026-09-29, MSVC 19.44.35227, x64, Release, from-scratch per-ref isolated
build dirs on post-merge `main` @ `8a3df91`):

| DuckDB | Build | Passed | Failed | Skipped | Assertions | Status |
|---|---|---:|---:|---:|---:|---|
| v1.5.4 (`08e34c44`) | ok | 51 | 0 | 1 | 1491 | PASS-WITH-SKIPS |
| v1.5.5 (`d8cdaa33`) | ok | 51 | 0 | 1 | 1491 | PASS-WITH-SKIPS |

The single skip is `salesforce_quota_concurrency.test`'s documented
conditional skip. `salesforce_agg_pushdown.test` passed on both refs
(42 assertions each).

### Official CI run record

`MainDistributionPipeline.yml` run
[36611832353](https://github.com/flozer/duckdb-salesforce/actions/runs/36611832353)
(2026-09-29, `workflow_dispatch` against post-merge `main` @ `8a3df91`,
pre-tag; extension-ci-tools `8d2a39a` matching the submodule) — **6/6
green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |

The feature branch's pre-merge run
[36604401384](https://github.com/flozer/duckdb-salesforce/actions/runs/36604401384)
was also 6/6 green.

## Gates

- Community update: **not submitted and not scheduled by this release** — per
  the maintainer's standing rule, any `duckdb/community-extensions` action
  requires a separate, explicit OK.
- Deferred follow-ups (roadmap): P1.2b `COUNT_DISTINCT`, P1.3 `MIN`/`MAX`,
  P1.4 `SUM`/`AVG` (gated on decimal evidence), official v1.5.6 support
  declaration (P4).

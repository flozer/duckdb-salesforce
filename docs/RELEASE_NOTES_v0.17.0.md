# duckdb-salesforce v0.17.0

> **Own-repo release.** The transparent aggregate pushdown introduced in
> v0.16.0 (roadmap P1.2a) now covers the full no-group aggregate family short
> of SUM/AVG: **`COUNT(DISTINCT col)`** (P1.2b) and **`MIN(col)`/`MAX(col)`**
> (P1.3) — same guarded `OptimizerExtension` rewrite, one server-side
> aggregate SOQL query, single-row emission. Kill-switch unchanged:
> `SET sf_aggregate_pushdown = false`.

## What changed

**`COUNT(DISTINCT)` pushdown (P1.2b)** — registers as SOQL
`COUNT_DISTINCT(field)`; result stays BIGINT.

**`MIN`/`MAX` pushdown (P1.3)** — result carries the field's DuckDB type.
Extra guards beyond the shared contract: the field must be **sortable** and
of a numeric, temporal or boolean DuckDB type. Strings stay local — SOQL vs
DuckDB collation semantics are not proven equivalent (spec §6.2). Mixed
terms (`SELECT COUNT(Id), MIN(Amount) ...`) run in ONE server-side query.

**Type-aware emission** — `SalesforceSession::TryAggregateQuery` returns the
raw aggregate record; the scan decodes each output through the existing
`AppendJsonValue` path using per-term pseudo-fields (name = the fixed SOQL
alias `a0..aN`). Nulls pass through as NULL — the correct empty-org answer
for MIN/MAX — while a null COUNT-family value and any transport or
result-shape failure remain hard errors (the plan has no local aggregate
left to fall back to).

**Mock router convention (test infra)** — pushed-aggregate queries are keyed
on the fixed `a0` output alias (alongside `COUNT`) instead of bare
`MIN(`/`MAX(` matching, which broke the explicit `salesforce_aggregate()`
function and the PK-chunking MIN/MAX(Id) probes (caught by the offline suite
before push; both keep routing to the data-query mock). Fixture convention
documented in the router: avoid the literal substring `a0` in canned WHERE
data.

**Still deferred (P1.4):** transparent `SUM`/`AVG` — gated on decimal
precision evidence. GROUP BY, aggregate-over-expression, relationship and
blob fields keep the normal row scan with local aggregation.

## Why v0.17.0 (minor)

New user-visible planner behavior behind the same default-on setting
(`sf_aggregate_pushdown`); loose policy: new feature = minor.

## Validation

- Local pinned build (submodule pin v1.5.6, MSVC/Ninja `Release`): full build
  green; **offline mock suite 51 files, 1513 assertions, 0 failures**.
- `clang-format` (project pin 11.0.1) clean over all touched files.
- Local `scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4` on
  the feature tree (`96344c6`, identical to the merged `main`): **both refs
  51 passed / 0 failed / 1 documented skip, 1514 assertions**,
  `salesforce_agg_pushdown.test` (65 assertions) passing on both.
- Official `MainDistributionPipeline.yml`: the feature branch ran
  [36624623428](https://github.com/flozer/duckdb-salesforce/actions/runs/36624623428)
  **6/6 green**; post-merge `main` run recorded below.

### Official CI run record

`MainDistributionPipeline.yml` run
[36629515473](https://github.com/flozer/duckdb-salesforce/actions/runs/36629515473)
(2026-09-29, `workflow_dispatch` against post-merge `main` @ `83289f0`,
pre-tag; extension-ci-tools `8d2a39a` matching the submodule) — **6/6
green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |

The feature branch's pre-merge run
[36624623428](https://github.com/flozer/duckdb-salesforce/actions/runs/36624623428)
was also 6/6 green.

## Gates

- Community update: **not submitted and not scheduled by this release** — per
  the maintainer's standing rule, any `duckdb/community-extensions` action
  requires a separate, explicit OK. (The v0.15.1 update
  [community-extensions#2850](https://github.com/duckdb/community-extensions/pull/2850)
  remains in upstream review; v0.16.0 and this release are NOT submitted.)
- Next planned (maintainer-approved queue): P4 — official DuckDB v1.5.6
  support declaration (compat release).

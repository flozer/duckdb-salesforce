# Changelog

All notable own-repo releases of `duckdb-salesforce`, newest first. This is a
**summary index** — each entry links to the full
[`docs/RELEASE_NOTES_*.md`](docs/) file for complete detail, evidence, and
validation records. The individual release notes are not reproduced here in
full and are not modified by this file's existence.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/); this
project does not yet follow strict Semantic Versioning (see
[docs/RELEASE_NOTES_v0.12.1.md](docs/RELEASE_NOTES_v0.12.1.md) for a patch
release that was purely a provenance/version-metadata fix, not a semver patch
in the strict sense).

## [v0.17.0](docs/RELEASE_NOTES_v0.17.0.md) — 2026-09-29

- **Aggregate pushdown family extended** (same `OptimizerExtension`,
  kill-switch `sf_aggregate_pushdown`): `COUNT(DISTINCT)` (SOQL
  `COUNT_DISTINCT`) and `MIN`/`MAX` join `COUNT` from v0.16.0. MIN/MAX
  require a sortable numeric/temporal/boolean field (strings stay local —
  collation unproven); mixed terms run in one server-side query.
- Type-aware single-row emission via the raw aggregate record and the
  existing decode path; MIN/MAX nulls pass through (empty-org answer),
  COUNT nulls and transport/shape failures stay hard errors.
- Mock-router convention: pushed aggregates keyed on the fixed `a0` alias;
  `salesforce_aggregate()` and PK-chunking probes keep their data-query
  routing (regression caught by the offline suite).
- SUM/AVG remain deferred (P1.4). Guard test: 65 assertions; suite: 51
  files, 1513 assertions, 0 failures; local matrix v1.5.4/v1.5.5
  1514 assertions each.

## [v0.16.0](docs/RELEASE_NOTES_v0.16.0.md) — 2026-09-29

- **Transparent `COUNT(field)` pushdown** (roadmap P1.2a, first
  `OptimizerExtension`): a no-group `COUNT(col)` directly over an attached
  sObject with zero residual filters is served by ONE server-side
  `SELECT COUNT(col) ... WHERE ...` query; the plan's Aggregate is replaced
  by a binding-preserving projection. Guarded (direct-Get child, no
  GROUP BY/DISTINCT/expressions, non-relationship/non-blob field, zero
  residual filters); kill-switch `sf_aggregate_pushdown` (default true).
  Transport/shape failure of the aggregate query is a hard error (no silent
  fallback). Diagnostics: last_soql/query_cost/query_explain gain aggregate
  visibility.
- New offline guard `test/sql/salesforce_agg_pushdown.test` (42 assertions);
  suite: 51 files, 1490 assertions, 0 failures.
- Build pins: `duckdb` submodule → tag v1.5.6 (#69 closed as wrong-target);
  `extension-ci-tools` → `8d2a39a` aligned with the workflow ref (PR #71,
  superseding #70). Official supported matrix unchanged (v1.5.4/v1.5.5).
- Minor bump per the loose policy (new feature = minor, precedents
  v0.9-v0.11).

## [v0.15.1](docs/RELEASE_NOTES_v0.15.1.md) — 2026-09-28

- **Function discoverability via `duckdb_functions()`** (issue
  [#66](https://github.com/flozer/duckdb-salesforce/issues/66)): all 25
  registered functions (24 table + 1 scalar) now carry a
  `FunctionDescription` — real parameter names (was `col0`/`col1` on
  11/14), a one-sentence description (was 0/25), a runnable example (was
  0/25), and categories.
- Registration-only metadata: **no scan/SOQL/pushdown/transport/quota
  behavior change**; `on_conflict` semantics of the bare overloads
  preserved (`ALTER_ON_CONFLICT`).
- New offline guard `test/sql/salesforce_functions_docs.test`.
- Patch bump per the `v0.12.1` precedent (binary-shipped docs, no
  functional change).

## [v0.15.0](docs/RELEASE_NOTES_v0.15.0.md) — 2026-09-15

- **DuckDB v1.5.2/v1.5.3 support dropped** — official baseline is now
  **v1.5.4+** (v1.5.2/v1.5.3 had a pre-existing, unresolved Windows/MSVC
  `fmt` build failure; scope reduction, not a code fix). User-visible
  change, the reason this is a minor bump and not a patch.
- `extension-ci-tools` pin aligned between `.gitmodules` and CI; stale
  `SECURITY.md` corrected.
- `.clang-format` added and all of `src/` reformatted (formatting only, no
  behavior change — same assertion count before/after).
- Informational, non-blocking ASan/UBSan/TSan CI jobs added; real
  concurrency test for the quota governor (`salesforce_quota_concurrency.test`,
  8 real threads).
- This `CHANGELOG.md` added; `docs/ARCHITECTURE.md` roadmap and Appendix
  A/B factual corrections (REST/Bulk crossover, GraphQL/Vault Mode marked
  historical).
- No SOQL/scan/transport/quota behavior change vs `v0.14.2`.
- Submitted as a community-extensions update:
  [duckdb/community-extensions#2710](https://github.com/duckdb/community-extensions/pull/2710)
  (pending review).

## [v0.14.2](docs/RELEASE_NOTES_v0.14.2.md) — 2026-08-28

- Compatibility-only release: DuckDB **v1.5.5** support, no functional
  `src/` changes.
- Official CI matrix green on Linux, Windows, and macOS (later reduced to
  v1.5.4/v1.5.5 only — see `docs/ROADMAP.md`/README for the v1.5.2/v1.5.3
  drop, 2026-09-14, post-dating this release).
- `release-assets.yml` restructured so a GitHub Release can never be
  partially published (atomic `publish` job gated on all platform builds).
- No DuckDB v2.0 compatibility claimed; `duckdb/main` canary evidence
  recorded separately.

## [v0.14.0](docs/RELEASE_NOTES_v0.14.0.md) — 2026-06-15

- `salesforce_relationship_graph()` gains opt-in **child** relationships
  (`include_children`) and a `direction := 'parent' | 'child' | 'both'`
  filter, schema-compatible with existing calls.
- Read-only, metadata-only; no scan/pushdown/transport behavior change.
- Own-repo release only; community catalog unaffected.

## [v0.13.0](docs/RELEASE_NOTES_v0.13.0.md) — 2026-06-15

- New `salesforce_relationship_graph(catalog, object [, max_depth])` —
  on-demand parent-relationship enumerator with explicit edge status
  (`resolved`, `polymorphic`, `self_reference`, `cyclic`, etc.).
- Report-type → base-object map contract clarified for the Report Bridge.
- Community update stays parked on the upstream Windows CI `fmt`/MSVC
  blocker (`community-extensions#2061`); own-repo asset build unaffected.

## [v0.12.1](docs/RELEASE_NOTES_v0.12.1.md) — 2026-06-14

- Provenance/patch release: no runtime change. Fixes a stale `vcpkg.json`
  `version-string` left over from the `v0.12.0` tag so version metadata,
  descriptor draft, and tag agree.
- Community submission ref re-pointed at `v0.12.1`; the live
  `docs/community/description.yml` itself stays untouched at `0.9.2`.

## [v0.12.0](docs/RELEASE_NOTES_v0.12.0.md) — 2026-06-13

- New `salesforce_query_explain()` — last-scan, field-by-field view of
  pushed vs. residual filters, projection, relationship, count, and
  transport decisions.
- Diagnostic-only; zero scan behavior change, zero output change to
  `salesforce_query_cost()`/`salesforce_report_soql()`.

## [v0.11.1](docs/RELEASE_NOTES_v0.11.1.md) — 2026-06-13

- **Metadata Engine v2**: a single, shared, per-catalog, read-only
  metadata cache backing both the Report Bridge and diagnostic functions,
  de-duplicating Describe Global/per-object Describe calls.
- Internal plumbing only — zero scan behavior change, zero
  `salesforce_report_soql()` output change.

## [v0.11.0](docs/RELEASE_NOTES_v0.11.0.md) — 2026-06-13

- `salesforce_report_soql()` gains describe-validated base-object
  resolution, token → field resolution, single-hop relationship support,
  and explainability columns (`translation_status`, `blocked_by`,
  `confidence`, etc.).
- No new SQL functions; richer, safer resolution of the existing one.

## [v0.10.1](docs/RELEASE_NOTES_v0.10.1.md) — 2026-06-12

- `salesforce_report_soql()` becomes fully describe-validated: base
  object, projected fields, and filtered fields are all checked against
  Describe before a candidate SOQL is ever emitted.
- Safety-first: when confidence is missing, returns
  `translatable = false` rather than a potentially wrong SOQL.

## [v0.10.0](docs/RELEASE_NOTES_v0.10.0.md) — 2026-06-12

- **Report Bridge** introduced: `salesforce_reports()`,
  `salesforce_report()`, `salesforce_report_soql()` — opt-in, read-only
  helpers bridging Salesforce Reports into DuckDB for discovery/
  validation, not large extraction (2,000-row synchronous cap).
- Community baseline unaffected (`v0.9.2`).

## [v0.9.3](docs/RELEASE_NOTES_v0.9.3.md) — 2026-06-12

- Fixes two-sided timestamp range pushdown (`BoundBetweenExpression`)
  that DuckDB could rewrite, previously causing over-fetch.
- Adds `sf_bulk_poll_budget` and `sf_bulk_require_predicate` Bulk
  backfill guardrails; `salesforce_query_cost()` reports `bulk_polls`.
- Offline suite: 1,968 assertions, 72 cases, 0 failures.

## [v0.9.2](docs/RELEASE_NOTES_v0.9.2.md) — 2026-06-05

- Operational/distribution release, no functional connector change.
- Adds the binary release-asset workflow (`release-assets.yml`) and
  packaging scripts; reinforces the "repo must be public" community
  preflight gate.

## [v0.9.1](docs/RELEASE_NOTES_v0.9.1.md) — 2026-06-05

- Adds `salesforce_refresh_metadata()` (manual cache invalidation) and
  `salesforce_picklist_values()`; documents a Bulk/blob compatibility
  guard and blob/datetime contracts.
- Read-only, no Metadata API.

## [v0.9.0](docs/RELEASE_NOTES_v0.9.md) — 2026-06-04

- `sf_query_mode = 'queryAll'` (archived/soft-deleted records) and
  explicit server-side `salesforce_aggregate()` (with `GROUP BY`).
- Richer authentication, relationship traversal + diagnostics, macOS TLS
  path.
- Tagged on `flozer` only; not published to community-extensions.

## [v0.8.0](docs/RELEASE_NOTES_v0.8.md) — 2026-06-03

- Distribution hardening, no new features: CI matrix on `flozer`
  (`MainDistributionPipeline.yml`), first proven Linux build,
  `docs/INSTALL.md` and `docs/PRE_COMMUNITY_CHECKLIST.md` added.
- Offline suite: 21 `test/sql/*.test` files green.

## [v0.7.1](docs/RELEASE_NOTES_v0.7.1.md) — 2026-06-03

- `sf_bulk_chunks=N` PK chunking now runs chunks **in parallel** instead
  of sequentially, each with its own client/session/Bulk job.
- Default `sf_bulk_chunks=1` behavior unchanged.

## [v0.7.0](docs/RELEASE_NOTES_v0.7.md) — 2026-06-03

- Lazy Bulk result streaming: result CSV pages fetch on demand instead of
  all up front, so a small `LIMIT` stops pulling early.
- Sequential PK chunking (`sf_bulk_chunks=N`, capped at 8) introduced.

## [v0.6.0](docs/RELEASE_NOTES_v0.6.md) — 2026-06-03

- Fast schema discovery via the Tooling API (`sf_schema_source='tooling'`),
  collapsing N per-object describes into one/few calls, with per-object
  fallback to REST describe on any gap.
- Parent relationship support added (§7).

## [v0.5.0](docs/RELEASE_NOTES_v0.5.md) — 2026-06-02

- `salesforce_query_cost()` introduced: a single-row, last-scan view of
  SOQL, transport, pushdown ratios, pages, and quota decision.
- `COUNT(*)` pushdown for zero-column count-class scans.

## [v0.4.0](docs/RELEASE_NOTES_v0.4.md) — 2026-06-02

- Bulk API 2.0 query path added (job creation, polling, CSV result
  decoding), sharing pushdown logic with the REST path.
- `sf_force_transport = 'rest' | 'bulk' | 'auto'` transport selection.
- Quota governor protecting Bulk job starts introduced.

## [v0.2.0](docs/RELEASE_NOTES_v0.2.md) — 2026-06-02

- Lazy/streaming REST scan (page-granularity fetch), metadata describe
  cache, global object listing.
- Broader predicate pushdown: `IN`, `LIKE`, `OR` as superset prefilters
  with residual safety.
- `scripts/build_matrix.ps1` build matrix introduced; verified against
  DuckDB v1.5.2 and v1.5.3, 262 offline assertions green.

---

Releases before `v0.2.0` (the `v0.1.0` scaffold) predate this changelog and
have no `docs/RELEASE_NOTES_*.md` file; see git history at tag `v0.1.0` for
that starting point.

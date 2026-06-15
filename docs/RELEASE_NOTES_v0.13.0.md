# duckdb-salesforce v0.13.0 — DRAFT (candidate, NOT tagged)

> **DRAFT / candidate.** Version `v0.13.0` is provisional and **not tagged**.
> This collects the work accumulated on `main` since `v0.12.1`. Tagging,
> release-assets, and any community update are **GO-gated**. The community
> candidate `v0.12.1` stays **frozen + parked** on the upstream Windows CI
> blocker (`duckdb/community-extensions#2061`); this draft does not change that.

Own-repo release on top of `v0.12.1` (`b5e769d`). Scope: Report-Bridge /
metadata diagnostics improvements. All additions are **read-only, diagnostic-
only**; no scan / pushdown / transport behavior change. Community baseline stays
`v0.9.2`.

## Highlights

### `salesforce_relationship_graph(catalog, object [, max_depth])` (new)

On-demand, read-only **parent** relationship enumerator (ROADMAP §18 cut 1),
sourced through the shared Metadata Engine. One row per edge with an explicit
status — `resolved`, `polymorphic` (all `referenceTo` targets reported, not
traversed), `self_reference` (direct), `cyclic` (longer path), `not_queryable`,
`not_describable`. Depth default `1`, clamped `[1,4]`. Distinct from the
last-scan `salesforce_relationships()`; never changes scan behavior. Columns:
`source_object, relationship_name, path, depth_level, target_object,
reference_to, direction, relationship_type, status, caveat`.

### Report-type → base-object map contract clarified (§16 Phase 1.1, cut 1)

`salesforce_report_soql()`'s standard report-type → base sObject map is now an
explicit **evidence-backed contract**: every entry is proven by a real report
(live smoke / fixture), validated queryable in Describe Global; an absent type
yields no candidate (base unresolved → block), never a guess. No
`reportTypeMetadata` inference. `column_prefix` stays hint-only.

### Compound/address token resolver — mechanism only

A report-side, object-keyed normalizer maps report builtin positional tokens
(`ADDRESS2_*`, `PHONE1-3`) to a candidate component field, **validated against
the Metadata Engine** (existence + filterability) before entering SOQL. **The
real token map is empty** — entries are added only with captured org fixture
evidence. **Not announceable as "ADDRESS2 resolved"** in this release; the
mechanism is in place, proven entries land later.

## Properties

- Read-only / diagnostic-only; no scan/pushdown/transport/SOQL behavior change.
- No partial SOQL: unresolved/ambiguous → conservative block with structured
  reasons.
- No Metadata API; metadata sourced via REST Describe through the shared engine.

## Changes since v0.12.1

- `feat(metadata)`: `salesforce_relationship_graph()` (§18 cut 1).
- `feat(report)`: compound/address token resolver mechanism (empty real map).
- `feat(report)`: report-type → base-object builtin map contract + TDD.

## Evidence (to complete before tagging)

- Offline mock suite green (full `*salesforce*`: 2768 assertions, 0 fail; 8 live
  tests gated/skipped).
- [x] Live maintainer smoke (PII-free) for `salesforce_relationship_graph()` —
      `salesforce_relationship_graph('sf','Contact',2)` on a real org exercised
      all edge statuses (resolved 35 / self_reference 21 / cyclic 4 /
      polymorphic 4). Evidence: `docs/smoke/relationship-graph-v0.13.0.md`.
- [ ] Fresh matrix CI at the eventual `v0.13.0` ref (linux + osx green; Windows
      subject to the same upstream `#2061` blocker — assess at tag time).

## Gates

- **Not tagged.** Tag/GitHub Release/assets are GO-gated.
- No community update; `docs/community/description.yml` stays `0.9.2`.

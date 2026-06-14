# duckdb-salesforce v0.12.0

Own-repo release on top of `v0.11.1` (`a6f8c20`). Scope: **scan explainability**
— the new `salesforce_query_explain()` table function (ROADMAP v1.6 §19),
completing the diagnostics/explainability arc that `v0.11.1` started with
Metadata Engine v2. Read-only, diagnostic-only, **zero scan behavior change** and
**zero `salesforce_query_cost()` / `salesforce_report_soql()` output change**.
The approved `duckdb/community-extensions` baseline remains `v0.9.2` — community
is **not** updated.

## Highlights

### `salesforce_query_explain()` — last-scan, field-by-field

A new read-only table function (no arguments) that explains the most recent
catalog scan, one row per projected field and per conjunctive filter, annotated
via the shared Metadata Engine v2:

| column | meaning |
|---|---|
| `object_name`, `field_name`, `role` | the scanned object; the field; `projection` \| `filter` \| `relationship` \| `count` \| `transport` |
| `resolved`, `filterable`, `sortable` | did the field resolve in metadata; its describe flags (nullable for meta rows) |
| `relationship_name`, `reference_to` | single-hop relationship + target(s) when applicable |
| `pushed`, `residual` | was it pushed into the SOQL (SELECT/WHERE); is DuckDB re-applying it |
| `reason`, `guidance` | closed reason token + a short actionable hint |

This answers the question that matters for daily use: **is Salesforce filtering
server-side, or is DuckDB filtering locally after dragging a full object across
the wire?** A `not_filterable` / `complex_expression` residual row is the
warning sign of over-fetch.

Closed `reason` set: `pushed_to_soql`, `projected`, `not_filterable`,
`unsupported_operator`, `complex_expression`, `unresolved_field`,
`metadata_unavailable`, `relationship_traversed`, `count_pushdown`,
`count_not_pushed`, `transport_rest`, `transport_bulk`.

Meta rows round out the picture: a `relationship` row per relationship the scan
actually traversed, one `count` row (`count_pushdown` | `count_not_pushed`), and
one `transport` row (`transport_rest` | `transport_bulk`, with reason / queryAll
/ est_rows in `guidance`).

### Diagnostics + Metadata Engine context (shipped in v0.11.1)

`salesforce_query_explain()` sits on top of the v0.11.1 foundation:
`salesforce_metadata_objects()`, `salesforce_metadata_fields()`, and the shared
per-catalog Metadata Engine v2 (de-duped Describe Global + per-object Describe,
`salesforce_refresh_metadata()` invalidation). Together they give analysts a real
metadata surface plus full query explainability.

## Properties (by design)

- **Read-only / diagnostic-only.** `query_explain()` reads a write-only snapshot
  the scan records; nothing in the scan path reads it back.
- **No fabrication.** With no scan in the session it returns **zero rows** (never
  a default `transport_rest`/`count_not_pushed`). LIMIT is intentionally absent —
  the table function never receives it in this DuckDB build, so it is not
  diagnosable rather than guessed.
- **Zero behavior change.** Scan execution, pushdown, transport selection,
  generated SOQL, and `salesforce_query_cost()` output are unchanged
  (regression-asserted).
- **Degrades, never throws.** If the owning catalog is detached / metadata
  unavailable, engine-annotated rows degrade to `metadata_unavailable`; the
  count/transport meta rows still emit.

## Changes since v0.11.1

- `feat(diag)`: `salesforce_query_explain()` Phase 1 — projection/filter rows,
  metadata-annotated, closed reason set (`68338c4`, merged `449fbd6`).
- `feat(diag)`: Phase 2 — relationship/count/transport meta rows (`ff1bff1`).
- `fix(diag)`: no-scan guard + `DiagReset()` on extension load — zero rows until a
  real scan, session-scoped (`f9bbed6`); merged `0489019`.

## Evidence

- Offline mock suite green (full `*salesforce*`: 2540 assertions, 0 fail; 8 live
  tests maintainer-gated/skipped in CI).
- Live maintainer smoke (PII-free): `docs/smoke/metadata-query-explain-v0.12.0.md`
  — `salesforce_metadata_objects` / `_fields`, a simple real query, then
  `salesforce_query_cost()` + `salesforce_query_explain()`, schema/diagnostics
  only, no record rows, no secrets.

## Gates

- Tag/GitHub Release and Linux/Windows assets are produced by the release-assets
  workflow on the `v0.12.0` tag — **only on explicit maintainer GO**.
- No community update; `docs/community/description.yml` stays `0.9.2`.

## Published assets

Release-assets workflow run `27484667321`: **completed / success**. The
`v0.12.0` GitHub Release carries exactly:

- `duckdb-salesforce-0.12.0-linux-x64.tar.gz`
- `duckdb-salesforce-0.12.0-windows-x64.zip`

Tag `v0.12.0` → `309d9ca`. Community catalog unchanged (`0.9.2`).

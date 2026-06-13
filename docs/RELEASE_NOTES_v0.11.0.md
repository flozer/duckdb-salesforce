# duckdb-salesforce v0.11.0 — DRAFT

> **DRAFT — not tagged/released.** Own-repo release on top of `v0.10.1`
> (`6deab9d`). Scope: the v1.6 Report Bridge metadata + explainability track
> (Phases 1–4) for `salesforce_report_soql()`. No new SQL functions; the change
> is richer, describe-validated resolution plus diagnostic columns. The approved
> `duckdb/community-extensions` baseline remains `v0.9.2` — community is **not**
> updated.

## Highlights

`salesforce_report_soql()` now resolves a report toward a candidate SOQL through
fully describe-validated steps, and explains itself:

- **Base object resolution** — the report type is resolved to a real, queryable
  sObject: `CustomEntity$X` → `X`, a small fixture-backed map for standard types
  (`ContactList`→`Contact`, `AccountList`→`Account`, `OpportunityList`→`Opportunity`),
  each validated as queryable in Describe Global. A column-prefix guess is a
  diagnostic hint only and never enables translation.
- **Token → field resolution** — report column/filter tokens are resolved to
  real field API names on the base sObject (`FIRST_NAME`→`FirstName`,
  `EMAIL`→`Email`, …) via case-insensitive match, a small fixture-backed token
  map, and `UPPER_SNAKE`→`PascalCase`, each confirmed against the Describe.
- **Single-hop relationships** — `Account.Name`, `Parent__r.Name` resolve when
  the prefix is a `relationshipName` on the base with exactly one `referenceTo`
  (non-polymorphic), the related object is queryable, and the final field exists
  (and is filterable for WHERE). Multi-hop and polymorphic relationships block.
- **Explainability columns** (appended; existing columns + `caveats` unchanged):
  `base_object_resolved_by`, `translation_status` (`full`|`none`), `blocked_by`
  (precedence-ranked closed set), `unresolved_columns`, `unresolved_filters`,
  `confidence` (`1.0`|`0.0`). A consumer can read exactly why `soql` is `NULL`
  without parsing caveats.

## Still conservative (by design)

- **No partial SOQL.** Any unresolved/ambiguous element → `translatable=false`,
  `soql=NULL`, with the structured reason. Better an honest false than a wrong
  query.
- WHERE fields must be `filterable`; cross filters, summary/matrix, OR/NOT/grouped
  filter logic, unsafe identifiers, and ambiguous literals all block.
- `salesforce_report()` remains the validated **sample/oracle** (≤2000 rows); it
  is not an extraction path. Scale through normal `sf.<Object>` scans.

## Changes since v0.10.1

- `feat(report)`: Phase 1 base-object resolver (`8a5a7c8`).
- `feat(report)`: Phase 2 token→field resolution (`b7fc3bc`).
- `feat(report)`: Phase 3 single-hop relationship fields (`b4ce26d`).
- `feat(report)`: Phase 4 explainability columns (`4c215b5`).
- `test(smoke)`: PII-safe `report()` + Phase 4 diagnostics in the runner.

## Evidence

- Offline mock suite green (full `*salesforce*`: 2264 assertions, 0 fail; live
  tests maintainer-gated/skipped in CI).
- Live maintainer smoke (PII-free): `docs/smoke/report-bridge-v0.11.0.md` —
  on a real `ContactList` report, base resolves to `Contact`, simple tokens
  resolve, an `Account.*` relationship resolves to the filterability check, and
  the structured diagnostics explain the `translatable=false` precisely.

## Gates

- Tag/GitHub Release and Linux/Windows assets are produced by the release-assets
  workflow on the `v0.11.0` tag (cut after smoke/CI review).
- No community update; `docs/community/description.yml` stays `0.9.2`.

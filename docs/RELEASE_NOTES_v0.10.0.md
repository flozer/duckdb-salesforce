# Release notes — v0.10.0

> **Report Bridge release.**
> v0.10.0 adds opt-in, read-only helpers that bridge Salesforce Reports into
> DuckDB discovery and validation workflows without turning the Reports API into
> an extraction path. The `duckdb/community-extensions` baseline remains
> `v0.9.2`; community publication is still blocked until Fernando gives a
> separate explicit GO.

## What's new

- **`salesforce_reports('sf')`** lists report definitions from the queryable
  `Report` sObject: `Id`, `Name`, `DeveloperName`, `FolderName`, `Format`.
- **`salesforce_report('sf', report_id)`** runs a synchronous tabular report as a
  validation sample and appends reserved diagnostics:
  `__sf_report_truncated`, `__sf_report_all_data`, `__sf_report_max_rows`, and
  `__sf_report_guidance`.
- **`salesforce_report_soql('sf', report_id)`** returns structured report
  ingredients (`base_object`, `columns`, `filters`) plus a conservative,
  best-effort candidate SOQL string when the report shape is safely
  translatable.

## Safety and limits

- Report execution is sample-only. Salesforce synchronous report data is capped
  at 2,000 rows and has no pagination; large extraction should still use normal
  `sf.<Object>` scans with REST/Bulk pushdown.
- Candidate SOQL is not an equivalence contract. Users must validate it against
  a `salesforce_report()` sample before scaling.
- Summary/matrix reports, unsupported operators, unsafe identifiers, ambiguous
  date/boolean/null literals, and complex report filter logic return
  `translatable = false` with a clear caveat and `soql = NULL`.
- Report labels that collide with reserved `__sf_report_*` diagnostics fall back
  to API names; duplicate labels are disambiguated with suffixes.

## Evidence

- **Focused Report Bridge tests**: `salesforce_reports.test`,
  `salesforce_report.test`, and `salesforce_report_soql.test`.
- **Offline mock suite**: 2,088 assertions, 78 cases, 0 failures; 8 live tests
  skipped by design because they are maintainer-gated.
- **Build**: clean release build reported by the implementation pass.

## Compatibility

- Default scan/query behavior remains unchanged.
- Report Bridge functions are opt-in and read-only.
- No live Salesforce tests run in CI; report coverage is offline/mock-only.
- No `duckdb/community-extensions` update is included in this release.

## Release assets

Tagging `v0.10.0` triggers `release-assets.yml` to publish the GitHub Release
`v0.10.0` using this file as the changelog, with platform assets for:

- `duckdb-salesforce-0.10.0-linux-x64.tar.gz`
- `duckdb-salesforce-0.10.0-windows-x64.zip`

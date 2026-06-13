# duckdb-salesforce v0.10.1

Patch release on top of `v0.10.0` (`a703447`). Scope: Report Bridge robustness +
safety for the candidate-SOQL path, plus smoke-tooling fixes. No new SQL surface;
existing behavior unchanged. The approved `duckdb/community-extensions` baseline
remains `v0.9.2` — this is an own-repo release only; community is **not** updated.

## Highlights

- **`salesforce_report_soql()` is now describe-validated.** The candidate SOQL is
  only produced when it can be trusted:
  - the base object is derived from the report type (`CustomEntity$X` → `X`) and
    accepted only when it **exists and is queryable** in Describe Global;
  - every **projected** field must exist on the sObject Describe;
  - every **filtered** field must exist **and** be `filterable = true` (a
    non-filterable field would make the SOQL fail at Salesforce);
  - cross filters, unsupported operators, and `OR`/`NOT`/grouped filter logic are
    rejected.
- **Safety first:** when confidence is missing, the function returns
  `translatable = false` with `soql = NULL` and an explaining caveat — it never
  emits a potentially wrong SOQL. The structured ingredients (`report_type`,
  `base_object`, `columns`, `filters`) are always returned.
- **Smoke runner improvements** (`scripts/run_smoke_report_bridge.ps1`):
  `-ListReports [-Format <fmt>]` listing mode, explicit `-ReportId`, UTF-8 console
  output, and CSV listing (no Unicode-border table) for readable evidence.

## What this release does and does not deliver

- **Delivered + validated in a real org:** `salesforce_report()` executes a real
  tabular report and returns a sample (≤2000 rows) with the reserved
  `__sf_report_*` diagnostics. This is the primary, working value.
- **Conservative candidate, by design:** `salesforce_report_soql()` returns safe,
  honest ingredients and a candidate SOQL. On real Salesforce reports,
  `translatable = true` is **rare today**: report types are not sObjects
  (`ContactList`/`AccountList`/`CustomEntity$…`) and report column tokens
  (`FIRST_NAME`, …) are not field API names. Broad report-type→sObject and
  token→field mapping is intentionally **out of scope** here.
- That mapping — a Metadata Engine / Relationship Resolver — is planned in the
  roadmap **v1.6 track (planning, not delivered)**.

## Changes since v0.10.0

- `feat(report): validate report_soql object and fields` — object + projected/
  filtered field validation against Describe / Describe Global; `filterable`
  guard for WHERE fields; no silent partial SOQL.
- `test(smoke): add Report Bridge report listing mode` — `-ListReports` /
  `-ReportId` / `-Format`.
- `fix(smoke): force UTF-8 + CSV output in report bridge runner`.
- `docs(roadmap): add core metadata and explainability track` — planning only.

## Evidence

- Offline mock suite green (full `*salesforce*`: 2112 assertions, 0 fail; live
  tests maintainer-gated/skipped in CI).
- Live maintainer smoke (PII-free): `docs/smoke/report-bridge-v0.10.1.md`.
- Release assets: workflow run `27449753390` (tag `v0.10.1` = `6deab9d`)
  completed **success**; GitHub Release `v0.10.1` published with
  `duckdb-salesforce-0.10.1-linux-x64.tar.gz` and
  `duckdb-salesforce-0.10.1-windows-x64.zip`.

## Gates

- Tag/GitHub Release and Linux/Windows assets are produced by the release-assets
  workflow on the `v0.10.1` tag.
- No community update; `docs/community/description.yml` stays `0.9.2`.

# Report Bridge — live smoke evidence (v0.10.1 candidate)

Post-merge maintainer smoke of the describe-validated `salesforce_report_soql()`
plus `salesforce_report()` against a real org. Evidence is **PII-free**: no
contact names, emails, phones, or report rows are reproduced here — only
structure and flags. Runner: `scripts/run_smoke_report_bridge.ps1`.

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-12T20:39–20:40 -03:00 |
| Git commit / main | `3c53d27` (no tag at HEAD; `v0.10.0` frozen at `a703447`) |
| duckdb shell | `build/release/duckdb.exe` (local Release build) — not community |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Mode | explicit (`-ReportId`) |

## Result: PASS

### Placeholder id → clean 403

First invocation used the literal placeholder `00O...` (not a real id).
`salesforce_report()` surfaced a clean, secret-free error:
`HTTP 403 FORBIDDEN — insufficient privileges`. Expected; error handling works.

### Real report `00OHY000000K6dJ2AS` → PASS

`salesforce_report('sf', '00OHY000000K6dJ2AS') LIMIT 5`:

- Returned **5 real rows** (values omitted — PII).
- Reserved diagnostic columns present and correct, observed **live**:
  `__sf_report_truncated = false`, `__sf_report_all_data = true`,
  `__sf_report_max_rows = 2000`, `__sf_report_guidance` set.
- This closes the earlier 0-row coverage gap (reserved columns now confirmed on
  a non-empty real sample).

**UTF-8:** accented column labels and values rendered correctly (no CP437/CP850
mojibake). The console UTF-8 + CSV fix is confirmed against real data.

`salesforce_report_soql('sf', '00OHY000000K6dJ2AS')`:

- `report_type` = `ContactList` (a standard report type, not an sObject).
- `base_object` = `ContactList`.
- `columns` / `filters` — returned as structured ingredients (10 columns incl.
  report tokens and `Account.*` relationship fields; 2 filters). Values omitted.
- `soql` = `NULL`
- `translatable` = `false`
- `caveats` = *"base object 'ContactList' was not found as a queryable object in
  Describe Global"*

The describe validation **correctly refused** to synthesize SOQL: `ContactList`
is a report-type name, not a queryable sObject, so no (wrong) candidate SOQL was
produced. Structured ingredients were still returned. This is the intended
conservative, never-wrong behavior.

## Findings (tracked, not blockers)

1. **Report type ≠ sObject.** Standard (`ContactList`/`AccountList`) and custom
   (`CustomEntity$…`) report types are not queryable objects, so the base object
   cannot be derived/validated from the type alone → `translatable=false`.
2. **Report column tokens ≠ field API names.** Columns arrive as report tokens
   (`FIRST_NAME`, `ADDRESS2_CITY`, …) and `Account.*` relationship fields, which
   do not resolve directly against the sObject Describe.

Net: on real orgs, `salesforce_report_soql().translatable` will usually be
`false` until report-type→sObject and report-token→field mapping exist. That is
the v1.6 Core Metadata / Relationship Resolver track — out of scope here. The
delivered value today is `salesforce_report()` (the validated sample/oracle) and
the safe, structured, honest `salesforce_report_soql()` ingredients.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`version: 0.9.2`. No PII recorded.

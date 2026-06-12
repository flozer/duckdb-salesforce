# Report Bridge — live smoke evidence (v0.10.0)

Post-release maintainer smoke of the §16 Report Bridge against a real org. This
is evidence captured **after** the `v0.10.0` tag (`a703447`); it does not change
the tag or the release notes. Runner: `scripts/run_smoke_report_bridge.ps1`.

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-12T19:47:43-03:00 |
| Git commit | `305f928` (smoke runner; on `main`, after tag `v0.10.0` = `a703447`) |
| duckdb shell | `build/release/duckdb.exe` (local Release build) |
| Extension | statically linked in shell — **not** community (community stays `v0.9.2`) |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Report selection | first `TABULAR` from `salesforce_reports('sf')` |
| Report tested | `00O4x000004sbbwEAA` — "IERs with Clicks (for an Email Send)" |

## Result: PASS

All three functions returned without error. No secret printed. No community
install. No stop condition triggered.

### `salesforce_reports('sf')` — PASS

Listed report definitions and selected a tabular report.

### `salesforce_report('sf', '00O4x000004sbbwEAA')` LIMIT 5 — PASS (0 rows)

Returned **0 rows**, therefore no diagnostic row — the **documented 0-row
behavior, confirmed live**. The report's own filters are contradictory
(`SendDefinition equals '' AND notEqual ''`), so an empty result is correct.
The reserved `__sf_report_*` diagnostic columns were not *observed* on this run
because there were no rows (they are proven by the offline mock test
`test/sql/salesforce_report.test`).

### `salesforce_report_soql('sf', '00O4x000004sbbwEAA')` — PASS (safety guard fired)

- `report_type` / `base_object` = `CustomEntity$et4ae5__IndividualEmailResult__c`
- `columns` / `filters` — returned as structured ingredients (LIST / LIST<STRUCT>)
- `soql` = `NULL`
- `translatable` = `false`
- `caveats` = *"base object 'CustomEntity$et4ae5__IndividualEmailResult__c' is not a safe SOQL identifier"*

The identifier-safety guard correctly refused to synthesize SOQL for an unsafe
base object (the `$` in the report-type name), kept the structured ingredients,
and explained why. This is the intended conservative, candidate-not-contract
behavior.

## Observations / follow-ups

1. **0-row coverage gap (run-only):** the selected report had no rows, so the
   reserved `__sf_report_*` columns and a non-empty sample were not observed
   live. A re-run with `SF_REPORT_ID` of a small, data-bearing, single-object
   tabular report would close this. Behavior is covered offline.
2. **`base_object` derivation is weak on real reports (enhancement):**
   `reportMetadata.reportType.type` returns the report type's internal name
   (`CustomEntity$…`), not the underlying sObject API name. Today this is **safe**
   because it yields `translatable = false`, but it means most real reports will
   be non-translatable. Tracked as a Report Bridge future enhancement in
   `docs/ROADMAP.md` (map report type → base sObject via report `/describe`
   metadata when possible).

## Gates

No retag, no new release, no community update. `docs/community/description.yml`
remains `version: 0.9.2`.

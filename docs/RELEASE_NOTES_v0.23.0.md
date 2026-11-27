# duckdb-salesforce v0.23.0

> **Report Bridge resolves real address and phone column tokens.** Minor
> release (new translation coverage). Supported DuckDB set unchanged:
> **{v1.5.6, v2.0.0, v2.0.1}**. No new function, setting, or write capability
> (28 functions, unchanged).

## What changed

- **Fixture-backed report column token map** (roadmap R-008,
  [PR #97](https://github.com/flozer/duckdb-salesforce/pull/97)). Salesforce
  report builtin column tokens such as `ADDRESS2_CITY` or `PHONE1` are not
  sObject field names. The resolver for them shipped in v0.11 with an empty
  real map, so every report with these columns was blocked
  (`blocked_by = 'projected_field'`). It now maps pairs proven against a real
  org:

  | Object | Token | Field |
  |---|---|---|
  | Contact | `ADDRESS2_CITY` | `MailingCity` |
  | Contact | `ADDRESS2_STATE` | `MailingState` |
  | Contact | `ADDRESS2_COUNTRY` | `MailingCountry` |
  | Contact | `ADDRESS2_COUNTRY_CODE` | `MailingCountryCode` |
  | Contact | `PHONE1` | `Phone` |
  | Contact | `PHONE2` | `Fax` |
  | Contact | `PHONE3` | `MobilePhone` |
  | User | `CELL` | `MobilePhone` |

  The evidence is metadata only: each report's describe `detailColumnInfo`
  label and data type, matched against the sObject Describe. No record data
  was read. Each mapped candidate is still validated against the Describe for
  existence, and for filterability when it is used in a filter. Unmapped tokens
  still block, and nothing is guessed.
- The test-only `sf_mock_report_token_map` setting now **replaces** the real
  map when it is non-empty. This keeps the resolver's mechanism tests
  independent of the real entries.
- **Gated live tests:**
  - `salesforce_oauth_live.test` now expects `ATTACH` to succeed. It still
    expected the v0.1 scanner stub error.
  - New `salesforce_sfdx_live.test` covers public connected apps end to end.
    It is gated on `SF_SFDX_AUTH_URL` and documented in `SMOKE.md`.
- **Docs:** `docs/ARCHITECTURE.md` updated:
  - the auth sources and the optional `client_secret`;
  - JWT bearer, shipped in v0.9.0 and previously described as "future";
  - milestone rows through v0.22.1;
  - the current open-work note.

  The function manual (EN/PT) lists the mapped tokens.

## Live evidence (maintainer org, 2026-11-27)

The org has 65 tabular reports. With the map in place, every `ADDRESS2_*`,
`PHONE1-3` and `CELL` token in them resolves. One ContactList report went
from blocked to `translation_status = 'full'` and executes. The remaining
blockers are unrelated tokens such as `OWNER_FULL_NAME`, `ACCOUNT_RECORDTYPE`,
`ACCOUNT.*` and `CUST_*`.

## Packaging notes

- Release assets are built against **DuckDB v1.5.6**, unchanged.
- DuckDB v2.0.0 / v2.0.1 users build from source against their ref, or install
  from the DuckDB community registry after the separate maintainer-approved
  descriptor update.
- Published v0.22.1 and earlier tags and releases remain untouched.

## Why v0.23.0 (minor)

New translation coverage changes `salesforce_report_soql()` results for
affected reports: they go from blocked to translatable. No supported-version
change and no removed behavior.

## Validation

**Local Windows/MSVC Release matrix** (`scripts/build_matrix.ps1`, isolated
build per ref, offline suite excluding `*_live.test`), run on the PR #97 head
`9aef383`:

| DuckDB | Passed files | Failed | Skipped | Assertions |
|---|---:|---:|---:|---:|
| v1.5.6 (`069cc9f9`) | 60 | 0 | 0 | 1710 |
| v2.0.0 (`d54cbbe3`) | 60 | 0 | 0 | 1710 |
| v2.0.1 (`8d78114e`) | 60 | 0 | 0 | 1710 |

PR CI run [46052790876](https://github.com/flozer/duckdb-salesforce/actions/runs/46052790876)
on `9aef383`: 9/9 platform jobs passed.

### Official CI run record

PENDING - filled in after the `main` run.

## Gates

- **Community update**: pending explicit C.5 GO. The live community descriptor
  continues to serve v0.22.1 until a separate maintainer-approved submission.

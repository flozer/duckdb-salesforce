# Report Bridge — live smoke evidence (v0.11.0 candidate)

Live maintainer smoke of the v1.6 Report Bridge (Phases 1–4) against a real org,
using the locally-built Release shell — not the community extension. PII-free:
no contact names, emails, phones, or report rows are reproduced (the runner
aggregates `salesforce_report()` to a count + reserved-column flags). Only
structure, schema tokens, and diagnostics are recorded.

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-13T15:01 -03:00 |
| Git commit | `5903dbf` (main; v1.6 Phases 1–4 = `4c215b5` + smoke-runner prep) |
| Shell | `build/release/duckdb.exe` (local Release; **rebuilt via `shell` target**) |
| Extension | statically linked local artifact — NOT community |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Report tested | `00OHY000000K6dJ2AS` (Contacts/Accounts report) |

## Result: PASS (functions execute; diagnostics correct)

### `salesforce_report()` — PASS (PII-safe)
`sample_rows=5`, `__sf_report_truncated=false`, `__sf_report_all_data=true`,
`__sf_report_max_rows=2000`. Report data rows were not printed.

### `salesforce_report_soql()` — structured, conservative, explained

| Column | Value |
|---|---|
| `base_object` | `Contact` |
| `base_object_resolved_by` | `builtin_report_type_map` |
| `translation_status` | `none` |
| `blocked_by` | `projected_field` |
| `translatable` | `false` |
| `soql` | `NULL` |
| `unresolved_columns` | `[ADDRESS2_CITY, ADDRESS2_STATE, ADDRESS2_COUNTRY, PHONE1, PHONE2, PHONE3]` |
| `unresolved_filters` | `[]` |
| `confidence` | `0.0` |

`caveats` (summarized): `ContactList -> Contact` resolved via builtin map; the
listed address/phone report tokens do not resolve to Contact fields; one filter
field (an `Account.*` relationship lookup) resolved as a single-hop relationship
but is not filterable.

## Phase-by-phase, validated live

- **Phase 1 (base):** `ContactList` → `Contact`, `resolved_by=builtin_report_type_map`
  (v0.10.1 returned "base object not found"). Fixed.
- **Phase 2 (tokens):** `FIRST_NAME/LAST_NAME/EMAIL` resolved (absent from the
  unresolved list); `ADDRESS2_*`/`PHONE1-3` honestly unresolved (report
  compound/address tokens, not Contact field API names).
- **Phase 3 (relationship):** the `Account.*` filter token resolved as a
  validated single-hop relationship (reached the filterability check rather than
  "does not resolve"), then blocked because that related field is not filterable.
- **Phase 4 (explainability):** `translation_status`, `blocked_by`
  (precedence: `projected_field` outranks `filterability`), `unresolved_columns`,
  `confidence` make the NULL `soql` self-explanatory without parsing caveats.

`translatable=false` here is honest and fully explained — never a wrong SOQL.

## Findings / follow-ups (non-blocking)

1. Relationship filterability caveat reads "not filterable on 'Contact'" but the
   field belongs to the related object (`Account`); cosmetic caveat wording, to
   refine later. Correctness (the block) is right.
2. Address/compound report tokens (`ADDRESS2_CITY`, …) are out of the current
   conservative token map; expanding mapping for compound fields is future work,
   not part of v1.6.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`version: 0.9.2`. No PII recorded.

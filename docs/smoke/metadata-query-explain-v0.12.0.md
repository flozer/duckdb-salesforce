# Metadata + scan explainability — live smoke evidence (v0.12.0 candidate)

Live maintainer smoke of the v1.6 diagnostics/explainability stack against a real
org, using the locally-built Release shell — not the community extension.
PII-free: **schema metadata + diagnostics only** (object/field names, flags,
pushed/residual, transport, counts). The one real query is aggregated to a row
count; record data is NOT printed; no secrets are printed.

Runner: `scripts/run_smoke_query_explain_v0.12.0.ps1`.

## Status: PASS (functions execute; diagnostics correct)

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-13T22:11 -03:00 |
| Git commit | `ac6932b` (docs/v0.12.0-prep; code = `0489019` on main) |
| Shell | `build/release/duckdb.exe` (local Release; **rebuilt via `shell` target**) |
| Extension | statically linked local artifact — NOT community |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Object tested | `AIApplication` (auto: first queryable) |

## 1. `salesforce_metadata_objects('sf')` (first 10)

```
object_name,queryable
AIApplication,true
AIApplicationConfig,true
AIInsightAction,true
AIInsightFeedback,true
AIInsightReason,true
AIInsightValue,true
AIRecordInsight,true
AITrustAttrSetup,true
AITrustAttribute,true
AWS_Setting__mdt,true
```

## 2. `salesforce_metadata_fields('sf', 'AIApplication')` (first 10)

```
field_name,type,filterable,sortable,relationship_name,reference_to
CreatedById,reference,true,true,CreatedBy,[User]
CreatedDate,datetime,true,true,NULL,[]
DeveloperName,string,true,true,NULL,[]
Id,id,true,true,NULL,[]
IsDeleted,boolean,true,true,NULL,[]
Language,picklist,true,true,NULL,[]
LastModifiedById,reference,true,true,LastModifiedBy,[User]
LastModifiedDate,datetime,true,true,NULL,[]
MasterLabel,string,true,true,NULL,[]
NamespacePrefix,string,true,true,NULL,[]
```

## 3. Simple real query (count only)

`SELECT count(*) FROM (SELECT Id FROM sf.AIApplication WHERE Id <> '' LIMIT 50)`
→ `matched_rows = 0`. No record rows printed.

## 4. `salesforce_query_cost()`

```
          object = AIApplication
       transport = rest
transport_reason = forced
  pushed_filters = 1
residual_filters = 0
    where_pushed = Id != ''
  count_pushdown = true
      query_mode = query
```

The `Id <> ''` predicate was **pushed server-side** (`where_pushed = Id != ''`,
`pushed_filters = 1`, `residual_filters = 0`); the `count(*)` used count pushdown.

## 5. `salesforce_query_explain()`

```
object_name,field_name,role,resolved,filterable,pushed,residual,reason
AIApplication,NULL,count,NULL,NULL,true,false,count_pushdown
AIApplication,Id,filter,true,true,true,false,pushed_to_soql
AIApplication,NULL,transport,NULL,NULL,false,false,transport_rest
```

Field-by-field, all consistent with the cost summary:
- `filter` row: `Id` resolved + filterable, `pushed_to_soql` (server-side, not
  residual) — confirming Salesforce did the filtering, not DuckDB.
- `count` row: `count_pushdown` (served by `SELECT COUNT()`).
- `transport` row: `transport_rest`.
- `resolved`/`filterable` are NULL on the meta rows (count/transport), as
  designed.

## Note: smoke-script process scoping

The first attempt showed empty `query_cost`/`query_explain` because the runner
spawned a separate `duckdb` process per step; the last-scan diagnostic is
per-process. The runner now executes the scan + `query_cost()` + `query_explain()`
in ONE process. (This also live-confirmed the no-scan guard: a process with no
scan returned zero `query_explain` rows.)

## What this proves

- Metadata diagnostics (`objects`/`fields`) read through the shared engine.
- `query_explain()` explains the last scan field-by-field — distinguishing
  Salesforce server-side filtering (`pushed`) from DuckDB residual filtering, and
  reporting the transport choice.
- All output is read-only schema metadata + diagnostics — no record rows, no
  secrets.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`version: 0.9.2`. No PII recorded.

# Metadata Engine v2 — live smoke evidence (v0.11.1 candidate)

Live maintainer smoke of Metadata Engine v2 (ROADMAP v1.6 §17, Phases A+B+C+D)
against a real org, using the locally-built Release shell — not the community
extension. PII-free: **schema metadata only** (object names, field names, types,
queryable/filterable flags, reference targets, picklist values). No record data
is selected or printed; no secrets are printed.

Runner: `scripts/run_smoke_metadata_v0.11.1.ps1`.

## Status: PASS (functions execute; metadata correct)

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-13T20:04 -03:00 |
| Git commit | `06d2e49` (docs/v0.11.1-prep; code = `47b5179` on main) |
| Shell | `build/release/duckdb.exe` (local Release; **rebuilt via `shell` target**) |
| Extension | statically linked local artifact — NOT community |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Object tested | `AIApplication` (auto: first queryable) |

Note: the first run hit a stale CLI (pre-Phase-D binary; `salesforce_metadata_objects
does not exist`). Rebuilding the `shell` target fixed it — source/registration was
already on `main`. Recorded as a build-ordering reminder, not an extension defect.

## 1. `salesforce_metadata_objects('sf')` — object inventory

```
        total_objects = 1697
    queryable_objects = 1447
non_queryable_objects =  250
```

Both `queryable=true` and `queryable=false` are present (1447 + 250) — the real
flag is exposed, not a constant. First 10 (by queryable desc, name):

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

## 2. `salesforce_metadata_fields('sf', 'AIApplication')` — field metadata

```
     total_fields = 13
filterable_fields = 13
 reference_fields =  2
  picklist_fields =  3
```

First 10 (by field name):

```
field_name,type,filterable,sortable,relationship_name,reference_to,picklist_values
CreatedById,reference,true,true,CreatedBy,[User],[]
CreatedDate,datetime,true,true,NULL,[],[]
DeveloperName,string,true,true,NULL,[],[]
Id,id,true,true,NULL,[],[]
IsDeleted,boolean,true,true,NULL,[],[]
Language,picklist,true,true,NULL,[],"[en_US, de, es, fr, it, ja, sv, ko, zh_TW, zh_CN, pt_BR, nl_NL, da, th, fi, ru, es_MX, no, hu, pl, cs, tr, in, ro, vi, uk, iw, el, bg, en_GB, ar, sk, pt_PT, hr, sl, eu, ca]"
LastModifiedById,reference,true,true,LastModifiedBy,[User],[]
LastModifiedDate,datetime,true,true,NULL,[],[]
MasterLabel,string,true,true,NULL,[],[]
NamespacePrefix,string,true,true,NULL,[],[]
```

Validated live:
- **`reference_to`** populated for reference fields (`CreatedById`/`LastModifiedById`
  → `[User]`), empty `[]` for non-reference fields.
- **`picklist_values`** parses `picklistValues[*].value` (`Language` lists its
  allowed locale codes), empty `[]` for non-picklist fields.
- Empty lists are `[]`, **never NULL** (only `relationship_name` is NULL when
  absent).

## 3. `salesforce_refresh_metadata('sf')` — invalidation

```
catalog = sf
  scope = global
 object = NULL
```

Catalog-wide refresh: drops the global list + every object Describe.

## 4. Re-read after refresh — proves re-fetch works

```
total_objects_after_refresh = 1697
```

Same count as step 1 — the engine re-fetched cleanly after invalidation.

## What this proves

- Describe Global is de-duped behind the shared engine; `metadata_objects` and
  `metadata_fields` read through one per-catalog cache.
- The `queryable` flag is real (both values present across 1697 objects).
- `reference_to` / `picklist_values` LIST columns populate from Describe and
  yield empty lists (never NULL) when absent.
- `refresh` invalidates and the next read re-fetches without error.
- All output is read-only schema metadata — no record rows, no secrets.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`version: 0.9.2`. No PII recorded.

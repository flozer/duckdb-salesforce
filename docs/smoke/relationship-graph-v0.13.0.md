# salesforce_relationship_graph() — live smoke evidence (v0.13.0 candidate)

Live maintainer smoke of `salesforce_relationship_graph()` (ROADMAP v1.6 §18
cut 1) against a real org, using the locally-built Release shell — not the
community extension. PII-free: **schema metadata only** (object names,
relationship names, target objects, per-edge status). No record data is read or
printed; no secrets are printed.

Runner: `scripts/run_smoke_relationship_graph.ps1`.

## Status: PASS (all edge statuses exercised on a real object graph)

## Run

| Field | Value |
|---|---|
| Timestamp | 2026-06-15T10:30 -03:00 |
| Git commit | `434b35d` (docs/next-release-prep; code = §18 on `main`) |
| Shell | `build/release/duckdb.exe` (local Release; rebuilt via `shell` target) |
| Extension | statically linked local artifact — NOT community |
| Org / login_url | `https://vitoriastone.my.salesforce.com` (env auth) |
| Call | `salesforce_relationship_graph('sf', 'Contact', 2)` |

Note: the first attempt (commit `3ab6a5c`) auto-picked `AIApplication` because the
runner's `-Object` parameter did not bind (param was `-RelObject`). Fixed in
`434b35d`; this run correctly used `object: Contact`.

## Graph (first 25 edges; schema metadata only)

```
path,relationship_name,target_object,depth_level,status
Account,Account,Account,1,resolved
Account.ActivityMetric,ActivityMetric,ActivityMetric,2,resolved
Account.CreatedBy,CreatedBy,User,2,resolved
Account.IdeCli__r,IdeCli__r,Account,2,self_reference
Account.IdeCpg__r,IdeCpg__r,Tabela_Global__c,2,resolved
Account.IdeEmp__r,IdeEmp__r,Account,2,self_reference
Account.IdeFil__r,IdeFil__r,Account,2,self_reference
Account.IdeLoc__r,IdeLoc__r,Account,2,self_reference
Account.IdeRep__r,IdeRep__r,Account,2,self_reference
Account.IdeTab__r,IdeTab__r,Tabela_Global__c,2,resolved
Account.IdeVen__r,IdeVen__r,Account,2,self_reference
Account.JigsawCompany,JigsawCompany,NULL,2,polymorphic
Account.LastModifiedBy,LastModifiedBy,User,2,resolved
Account.MasterRecord,MasterRecord,Account,2,self_reference
Account.OperatingHours,OperatingHours,OperatingHours,2,resolved
Account.Owner,Owner,User,2,resolved
Account.Parent,Parent,Account,2,self_reference
Account.RecordType,RecordType,RecordType,2,resolved
Account.rh2__Describe__r,rh2__Describe__r,rh2__PS_Describe__c,2,resolved
ActivityMetric,ActivityMetric,ActivityMetric,1,resolved
ActivityMetric.Base,Base,NULL,2,polymorphic
CreatedBy,CreatedBy,User,1,resolved
CreatedBy.Account,Account,Account,2,resolved
CreatedBy.Contact,Contact,Contact,2,cyclic
CreatedBy.CreatedBy,CreatedBy,User,2,self_reference
```

## Status summary (counts only)

```
status = cyclic           edges = 4
status = polymorphic      edges = 4
status = resolved         edges = 35
status = self_reference   edges = 21
```

## What this proves (live, on a real object graph)

- **`resolved`** — single-`referenceTo`, queryable parents expand (Account, User,
  Profile, UserRole, custom `__r` parents like `IdeTab__r`→`Tabela_Global__c`).
- **`self_reference`** — direct self-parents flagged, not recursed
  (`Account.Parent`→`Account`, custom `IdeEmp__r`/`IdeCli__r`→`Account`).
- **`cyclic`** — a cycle via a longer path is detected and stopped:
  `Contact → CreatedBy(User) → Contact` → `CreatedBy.Contact` = `cyclic`
  (distinct from `self_reference`, exactly as designed).
- **`polymorphic`** — multi-`referenceTo` edges reported with `target_object`
  NULL, not traversed (`Account.JigsawCompany`, `ActivityMetric.Base`).
- Depth bound honored (`max_depth=2`: edges at depth 1 and 2 only).
- Output is read-only schema metadata — no record rows, no secrets.

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`0.9.2`. v0.13.0 remains a DRAFT candidate (`docs/RELEASE_NOTES_v0.13.0.md`).

# Release Notes — v0.6.0

> **STATUS: VALIDATED — tagged `v0.6.0`.** Validated by maintainer-provided
> manual live smoke evidence against an authorized org, plus a green offline
> suite. The DEV did **not** independently rerun the live smoke (no `SF_LIVE_*`
> credentials were present); the live validation is attributed to the
> maintainer's reviewed evidence. Covers the v0.6 §6 (Tooling fast schema) + §7
> (parent relationship support) work on `flozer/duckdb-salesforce` `main`.
> Nothing in this cycle touches `duckdb/community-extensions` (gate C.5).

Commits: `f0710b0` (§6 Tooling), `32cddcf` (§7 relationships).

---

## What's new

### Fast schema discovery — Tooling API (§6)

By default each sObject's schema comes from one **REST describe** (authoritative).
For orgs where you touch many tables, `sf_schema_source = 'tooling'` switches
discovery to the **Tooling API** (`FieldDefinition`), fetching the fields of
**many objects in one query** — collapsing N describes into one/few calls.

```sql
SET sf_schema_source = 'tooling';   -- opt-in; default 'describe'
SELECT * FROM sf.Account;
SELECT calls FROM salesforce_tooling_calls();   -- proves Tooling use / batching
```

- **Per-object fallback to REST describe** on any gap: Tooling HTTP error, the
  object absent from the result, or an ambiguous/unmapped `DataType`
  (`Formula`, `Roll-Up Summary`, unknown). REST describe stays authoritative.
- **Coarser types** — the Tooling `DataType` display string is mapped
  best-effort (`Text(255)`→VARCHAR, `Number(18,0)`→DECIMAL, `Checkbox`→BOOLEAN,
  `Date/Time`→TIMESTAMP, …).
- **Reduced pushdown** — a field is pushable only if Tooling marks it filterable;
  otherwise predicates stay residual (correct, just less pushdown).
- Compound fields dropped, same as describe. Default stays `'describe'`.

### Relationship support — parent traversal (§7)

Opt-in parent (lookup / master-detail) traversal. With `sf_relationships =
'parent'`, each **single-target** parent relationship becomes a **STRUCT column**
named by its Salesforce relationship name:

```sql
SET sf_relationships = 'parent';    -- opt-in; default 'off'
SELECT Id, Account.Name FROM sf.Contact;
-- Contact gains  Account STRUCT(Id, Name, ...);  SOQL uses Account.Name
```

- **Default `off`** → schema and `SELECT *` unchanged unless opted in.
- A null/missing parent → null struct; a missing subfield → null.
- Polymorphic relationships (e.g. `OwnerId` → User/Group) are **skipped**.
- Parent describe reuses the per-attach cache.

### New diagnostics

```sql
SELECT calls FROM salesforce_tooling_calls();   -- Tooling schema queries since ATTACH
```

---

## Limitations

Tooling (`sf_schema_source='tooling'`):

- Coarser types; ambiguous/unmapped → per-object REST fallback.
- Conservative filterability → reduced pushdown.
- Tooling path is field-discovery only; object listing still uses global describe.

Relationships (`sf_relationships='parent'`):

- **Parent only, depth 1.** No grandparent (`Account.Owner.Name`), no child
  subqueries, no relationship fan-out — use DuckDB joins for those.
- **No polymorphic** relationships.
- **Subfield predicates are residual** — `WHERE Account.Name = …` is applied by
  DuckDB, not pushed to SOQL, in this cut.
- Selecting the struct **over-fetches** all parent scalar fields.
- Describe-source only (not combined with Tooling) in this cut.

All prior limitations stand (read-only; Bulk ignores `LIMIT` + eager fetch;
`auto` cannot see `LIMIT`; quota governor gates Bulk starts only; COUNT pushdown
is COUNT-only).

---

## Smoke checklist (manual, maintainer-only)

Run against a user-authorized org controlled by the maintainer. Automated CI
must never contact Salesforce or require secrets.

- [ ] **Default off** — with no settings changed, `SELECT *` / `DESCRIBE` on an
      object is unchanged (no STRUCT columns); `salesforce_tooling_calls()` = 0.
- [ ] **Tooling** — `SET sf_schema_source='tooling';` then query a few objects;
      `salesforce_tooling_calls()` > 0 and far fewer than the object count;
      schemas look right (spot-check types); an object with a Formula field
      still resolves (REST fallback).
- [ ] **Relationship** — `SET sf_relationships='parent';
      SELECT Id, Account.Name FROM sf.Contact LIMIT 5;` returns parent names;
      `salesforce_last_soql()` shows `Account.Name`; a Contact with no Account →
      NULL.

---

## Validation record (for tagging)

Gate status at tag time — all satisfied:

- [x] Offline test suite green — 19 `test/sql/*.test` files, verified by the DEV.
- [x] Manual smoke: default-off (normal `Id, Name` rows, no STRUCT) — maintainer-attested.
- [x] Manual smoke: Tooling fast schema (`salesforce_tooling_calls()` = 1) — maintainer-attested.
- [x] Manual smoke: relationship `Contact.Account.Name` — maintainer-attested.
- [x] Human go — maintainer authorized the tag.

Maintainer smoke evidence (secret-free; reviewed from live captures against an
authorized org):

- `DESCRIBE sf.Contact` shows `Account` as `STRUCT(...)` (and other single-target
  parents: `Owner`, `CreatedBy`, `LastModifiedBy`) → `sf_relationships='parent'`
  active.
- `SELECT … (Account).Name AS AccountName …` returned real parent names
  (e.g. "CUMAR INC", "RIO STONES INC", "GREENE MARBLE & GRANITE CO.",
  "LOUISIANA STONE LLC") with no errors → struct field access works.
- A normal query returned flat `Id, Name` (no struct) → default/off flow intact.
- `salesforce_tooling_calls()` = 1 from the prior Tooling capture → Tooling
  fast-schema active.

Attribution / honesty notes:

- The live smoke was **not** independently rerun by the DEV — no `SF_LIVE_*`
  credentials were present. The live validation is the **maintainer's** reviewed
  evidence.
- No secrets, tokens, org identifiers, or response bodies are recorded here
  (parent account names above are maintainer-approved sample output).
- Automated CI never contacts Salesforce and never requires secrets.

Tag: annotated `v0.6.0` on `flozer/duckdb-salesforce`. **Nothing** pushed to
`duckdb/community-extensions` (gate C.5). v0.7 (CI Win/Linux + package/release
review) comes only after the tag.

# Release Notes — v0.6.0 (DRAFT)

> **STATUS: DRAFT — NOT TAGGED.** Covers the v0.6 §6 (Tooling fast schema) +
> §7 (parent relationship support) work landed on `flozer/duckdb-salesforce`
> `main`. Do not tag until the **Before tagging** checklist is complete and a
> human gives the go. Nothing in this cycle touches
> `duckdb/community-extensions` (gate C.5).

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

## Before tagging

All must be true before tagging `v0.6.0`:

- [ ] Offline test suite green (all `test/sql/*.test`).
- [ ] Manual smoke: default-off (schema unchanged).
- [ ] Manual smoke: Tooling fast schema.
- [ ] Manual smoke: relationship `Contact.Account.Name`.
- [ ] Human go.

Only then: tag `v0.6.0` on `flozer/duckdb-salesforce`. **Nothing** in
`duckdb/community-extensions` (gate C.5). v0.7 (CI Win/Linux + package/release
review) comes only after the tag.

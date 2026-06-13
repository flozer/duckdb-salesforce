# Report Bridge v1.6 — Phase 3: Single-Hop Relationship Fields (design)

Status: DESIGN ONLY. No C++ in this branch. Scope: `salesforce_report_soql()`
**single-hop relationship** field resolution (`Account.Name`, `Owner.Name`,
`Parent__r.Name`). Builds on Phase 1 (base resolver) + Phase 2 (token→field).

## Goal

Resolve a report token whose prefix is a **relationship name** on the base
object to a safe single-hop SOQL traversal (`SELECT Account.Name FROM Contact`).
The prefix is a `relationshipName` from the base Describe — NOT an object name.
Only emit when the whole hop is validated; otherwise `translatable=false`.

## Resolution (token `Rel.Field`, single dot, prefix ≠ base)

1. In the base sObject Describe, find a field whose `relationshipName` equals the
   token prefix (case-insensitive), e.g. `Contact.AccountId` has
   `relationshipName = "Account"`; `Parent__r` comes from a `__c` lookup with
   `relationshipName = "Parent__r"`.
2. That field's `referenceTo` must contain **exactly one** object (non-
   polymorphic). Multiple targets → block.
3. The related object must be queryable in Describe Global and describable.
4. The final field (`Field`) must exist on the **related** object's Describe.
5. For a WHERE relationship field, the final field must also be
   `filterable = true` on the related object.
6. Emit the traversal using the relationship name + final field API name:
   `Rel.Field` (e.g. `Account.Name`). SELECT: `SELECT Account.Name FROM Contact`.

Any miss at any step → `translatable=false`, `soql=NULL`, caveat naming the token
and the reason. The final field on the related object is itself resolved with the
Phase 2 token→field rules (as-is / builtin map / normalization, confirmed on the
related Describe).

## Hard limits (block, do not invent)

- **Single hop only.** `Account.Owner.Name` (two dots) → block (later phase).
- **No invented joins**, no SQL JOIN, no child→parent reverse subquery, no
  child relationship (`(SELECT ... FROM Contacts)`).
- **Polymorphic** relationship (`referenceTo` size > 1, e.g. `OwnerId` →
  `[User, Group]`, `WhoId`, `WhatId`) → block. (A fixture-backed explicit rule
  could relax specific cases later; block now.)
- Prefix that matches no `relationshipName` on the base → block (it is not a
  bare object name; do not treat it as one).

## Tokens

- `Account.Name` → prefix `Account` must match a base field `relationshipName`.
- `Owner.Name` → same; blocked while `OwnerId` is polymorphic
  (`referenceTo=[User, Group]`); allowed only if `referenceTo` is exactly one.
- `CustomParent__r.Name` → prefix `CustomParent__r` matches a `__c` lookup with
  `relationshipName = CustomParent__r`, `referenceTo = [Parent__c]`.
- The prefix is always a `relationshipName`, never assumed to be an object name.

## Describe usage / cost

Phase 3 adds, per distinct related object, one extra `Describe(relatedObject)`
call (cache per object within the bind). Base Describe already carries
`relationshipName` + `referenceTo` per field (`SalesforceField`).

## TDD cases (mock-first)

1. `ContactList`, column `Account.Name`; base `Contact` has `AccountId`
   (`relationshipName=Account`, `referenceTo=["Account"]`); `Account` queryable
   with `Name` → `SELECT Account.Name FROM Contact`, `translatable=true`.
2. filter `Account.Name equals 'Acme'`, `Account.Name` filterable on `Account` →
   `WHERE Account.Name = 'Acme'`.
3. relationshipName not found on base → block.
4. `referenceTo` size > 1 (polymorphic) → block.
5. related field absent on related Describe → block.
6. multi-hop `Account.Owner.Name` (two dots) → block.
7. custom `Parent__r.Name` validated (`relationshipName=Parent__r`,
   `referenceTo=[Parent__c]`, `Parent__c.Name` exists) → allow.

## Acceptance

- A real `ContactList` report with `Account.*` columns can now translate when the
  hop validates; ambiguous/polymorphic/multi-hop stay `translatable=false`.
- Offline mock + full `*salesforce*` green. No live tests in CI.

## Out of scope (Phase 3)

- Multi-hop traversal, child subqueries, polymorphic relaxation, explainability
  columns (Phase 4). No schema change; provenance in caveats. No docs EN/PT yet
  (design spec only). No tag/release/community; `description.yml` stays `0.9.2`.

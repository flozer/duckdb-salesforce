# salesforce_relationship_graph() child relationships — live smoke evidence (§18 cut 2)

Live maintainer smoke of the **opt-in child relationships** path
(`include_children := true`, ROADMAP v1.6 §18 cut 2) against a real org, using
the locally-built Release shell — not the community extension. PII-free: schema
metadata only (object / relationship / target names + edge status). No record
data is read or printed; no secrets are printed.

Runner: `scripts/run_smoke_relationship_graph.ps1 -Object Account -MaxDepth 2 -IncludeChildren`.

## Status: PASS (maintainer-confirmed)

The maintainer ran the command above on a real org and confirmed, on the
PII-free output:

- header shows `children : True`;
- the query used `salesforce_relationship_graph('sf', 'Account', 2, include_children := true)`;
- **`direction=child` rows present**, exercising all child statuses live:
  `resolved`, `not_queryable`, and `unnamed_child` (a child relationship with a
  null `relationshipName` — reported, not an error);
- **parent** statuses also exercised on the same object: `resolved`,
  `self_reference`, `cyclic`, `polymorphic`;
- the summary splits counts **per direction + status**;
- output is read-only schema metadata — no record rows, no PII.

(Parent-only behavior was separately captured for v0.13.0 in
`docs/smoke/relationship-graph-v0.13.0.md`; this run adds the child path.)

## Offline-mock reproduction (this repo, no org needed)

The same CLI binary the maintainer ran was verified offline with the `sf_mock_*`
hooks (no credentials), confirming the `include_children` named param binds and
the child path executes end-to-end:

```sql
-- mock: Account with a parent ref (Owner->User) + childRelationships
--       [Contacts->Contact (queryable), Attachment (relationshipName null)]
SELECT direction, count(*) AS edges
FROM salesforce_relationship_graph('sf','Account', include_children := true)
GROUP BY direction ORDER BY direction;
```

```
┌───────────┬───────┐
│ direction │ edges │
├───────────┼───────┤
│ child     │     2 │   -- Contacts (resolved) + Attachment (unnamed_child)
│ parent    │     1 │   -- Owner -> User (resolved)
└───────────┴───────┘
```

## What this proves

- `include_children` is opt-in; default (omitted) stays parent-only and
  byte-identical to cut 1.
- Child relationships are listed for the root object (single level), with
  `direction=child`, `relationship_type=childRelationship`, and the full status
  set — including `unnamed_child` for null-`relationshipName` children.
- Live, on a real Account graph, both parent and child sides are exercised with
  no record data and no secrets.

## Direction filter — live (§18, `direction := 'child'`)

Second maintainer run (commit `a838381`) on the same real `Account`, exercising
the new `direction` filter:
`scripts/run_smoke_relationship_graph.ps1 -Object Account -Direction child`.

- header `direction : child`; query used
  `salesforce_relationship_graph('sf', 'Account', 2, direction := 'child')`;
- **child-only output — zero `direction=parent` rows** (the filter excludes
  parents);
- per-direction/status summary (PII-free, counts only):

```
┌───────────┬───────────────┬───────┐
│ direction │    status     │ edges │
├───────────┼───────────────┼───────┤
│ child     │ not_queryable │ 7     │
│ child     │ resolved      │ 131   │
│ child     │ unnamed_child │ 64    │
└───────────┴───────────────┴───────┘
```

For comparison, the same object with `-IncludeChildren` (both sides) added the
parent rows: `parent` resolved 37 / self_reference 18 / cyclic 8 / polymorphic 6.
So `direction := 'child'` returns exactly the child subset, no parent noise —
confirming the filter live. (Real org has 202 child relationships on Account,
incl. 64 unnamed and 7 non-queryable — all reported, none guessed.)

## Gates

No tag, no release, no community update. `docs/community/description.yml` stays
`0.9.2`; community candidate `v0.12.1` frozen + parked on `#2061`. This work
accrues to a future own-repo release (provisional `v0.14.0`).

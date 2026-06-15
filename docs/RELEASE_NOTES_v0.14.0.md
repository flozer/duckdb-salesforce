# duckdb-salesforce v0.14.0

> **Own-repo release.** This is an own-repo tag/release only. The **community
> update stays PARKED** on the upstream Windows CI blocker
> (`duckdb/community-extensions#2061`) — that blocker affects the
> community-extensions from-source DuckDB build, **not** this own-repo release
> (`release-assets.yml` pins `windows-2022`, which builds + publishes our Windows
> asset). Community baseline stays `v0.9.2`; the frozen community candidate is
> still `v0.12.1`. No community submission is made by this release.

Own-repo release on top of `v0.13.0` (`57a38ff`). Scope: relationship-graph
usability — child relationships + a direction filter. All additions are
**read-only, metadata-only**; no scan / pushdown / transport behavior change.

## Highlights

### `salesforce_relationship_graph()` — child relationships (§18 cut 2)

Opt-in child (one-to-many) relationships, via the named param
`include_children := false` (default OFF → **byte-identical** parent-only
output). When ON, the queried object's **direct** child relationships are listed
(single level, not recursed — child relationships fan out heavily):
`direction='child'`, `relationship_type='childRelationship'`,
`target_object=childSObject`, `reference_to=[back-FK field]`. Statuses:
`resolved` | `not_queryable` | `unnamed_child` (a child relationship with a null
`relationshipName` — not SOQL-subquery-addressable; reported, not an error).
`childRelationships` is parsed additively into Describe.

### `salesforce_relationship_graph()` — direction filter (§18)

New named param `direction := 'parent' | 'child' | 'both'` (case-insensitive),
**schema unchanged**:

- **Default stays `parent`-only** — children are never emitted silently.
- `direction` **wins** over `include_children` when both are given;
  `include_children` is kept as a backward-compatible alias
  (`include_children := true` ≡ `direction := 'both'`).
- `direction := 'child'` lists the root's child relationships only (no parent
  noise); `max_depth` applies to parent traversal, child rows are root-level.
- Invalid `direction` → a clear `BinderException`.

## Properties

- Read-only / metadata-only; no scan/pushdown/transport/Report Bridge change.
- Existing calls behave exactly as before (default parent-only; `include_children`
  unchanged).
- Sourced through the shared Metadata Engine (REST Describe); no Metadata API.

## Changes since v0.13.0

- `feat(metadata)`: child relationships in `salesforce_relationship_graph`
  (opt-in `include_children`).
- `feat(metadata)`: `direction := parent|child|both` filter.
- `docs`: EN/PT function-manual section for `salesforce_relationship_graph`
  (signature, columns, statuses, examples); smoke runner gains `-IncludeChildren`
  and `-Direction`.

## Evidence

- Offline mock suite green (full `*salesforce*`: 2888 assertions, 0 fail; 8 live
  tests gated/skipped).
- Live maintainer smoke (PII-free), real org `Account`:
  - `-IncludeChildren` (both) — parents resolved/self_reference/cyclic/polymorphic
    + 202 child relationships (resolved 131 / not_queryable 7 / unnamed_child 64).
  - `-Direction child` — child-only, zero parent rows.
  Evidence: `docs/smoke/relationship-graph-children-v0.14.0.md`.
- Matrix CI: `linux_amd64` + `osx_arm64` green; `windows_amd64` red **only** from
  the upstream `#2061` from-source DuckDB break (does not affect this release's
  Windows asset, built on `windows-2022`).

## Gates

- No community update; `docs/community/description.yml` stays `0.9.2`. The
  community candidate `v0.12.1` remains frozen + parked on `#2061`.

## Published assets

Release-assets workflow run `27558089483`: **completed / success** (2026-06-15).
The `v0.14.0` GitHub Release carries exactly:

- `duckdb-salesforce-0.14.0-linux-x64.tar.gz`
- `duckdb-salesforce-0.14.0-windows-x64.zip`

Tag `v0.14.0` → `21e61c4`. **Own-repo release only** — the Windows asset builds
on `windows-2022`, unaffected by the upstream `#2061` blocker. The community
update stays parked on `#2061`; community catalog unchanged (`0.9.2`).

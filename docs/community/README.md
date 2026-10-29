# Community-extensions package (LIVE at v0.20.2)

This folder holds the `duckdb/community-extensions` descriptor for
`duckdb-salesforce`.

**The extension is accepted and live in community at `v0.20.2`** — added via
[`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037)
(*Add salesforce extension*, MERGED 2026-06-09) and updated through
[`duckdb/community-extensions#3418`](https://github.com/duckdb/community-extensions/pull/3418)
(*Update salesforce extension to 0.20.2*, MERGED 2026-10-29). Users can install it today:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

## Files

- [`description.yml`](description.yml) — the **live** descriptor mirror
  (`version: 0.20.2`, `repo.ref: 0c9c2620d7e0479cf9d499674187cd82bcb030d4`).
  This is what community CI builds from today.
- [`description.v0.20.2.draft.yml`](description.v0.20.2.draft.yml) — the
  submitted and merged v0.20.2 descriptor, kept for audit trail.
- Older `description.v*.draft.yml` files and C5/C6 plans are historical
  submission records; do not promote them over the live mirror.

## Community update flow

A community **update** edits **one file** in a fork of
`duckdb/community-extensions` (the path already exists from #2037):

```
extensions/salesforce/description.yml
```

It receives the current `description.yml`. Nothing else changes there —
community CI clones `flozer/duckdb-salesforce` at the descriptor's `repo.ref`
and rebuilds + re-signs from our own `Makefile` / `extension_config.cmake` /
`vcpkg.json`.

## Update steps

1. Validate the new own-repo tag against target DuckDB releases.
2. Fork / update fork of `duckdb/community-extensions`.
3. Edit `extensions/salesforce/description.yml` = the current descriptor.
4. Open an **update** PR against `duckdb/community-extensions:main`.
5. Their CI rebuilds + re-signs for each non-excluded platform from `repo.ref`.
6. After maintainer merge, promote the live mirror, update public status docs,
   and smoke-test `INSTALL salesforce FROM community` in an isolated extension
   directory.

## Readiness

See [PR_READINESS.md](PR_READINESS.md) for the pre-PR checklist and
[../PRE_COMMUNITY_CHECKLIST.md](../PRE_COMMUNITY_CHECKLIST.md) for the full audit.

Current live ref: **v0.20.2**. Validation covered DuckDB `v1.5.4`, `v1.5.5`,
`v1.5.6`, and `v2.0.0` across Linux, Windows, and macOS arm64 in own-repo CI;
community CI passed its Linux and Windows builds before merge. New DuckDB
releases require explicit validation.

## Guardrails (C.5)

- No update PR, fork branch, or push to `duckdb/community-extensions` without
  explicit maintainer GO.

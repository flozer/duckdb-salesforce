# Community-extensions package (LIVE at v0.14.1)

This folder holds the `duckdb/community-extensions` descriptor for
`duckdb-salesforce`.

**The extension is accepted and live in community at `v0.14.1`** — added via
[`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037)
(*Add salesforce extension*, MERGED 2026-06-09) and updated via
[`duckdb/community-extensions#2078`](https://github.com/duckdb/community-extensions/pull/2078)
(*Update salesforce to v0.14.1*, MERGED 2026-06-19). Users can install it today:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```


## Files

- [`description.yml`](description.yml) — the **live** descriptor mirror
  (`version: 0.14.1`, `repo.ref: v0.14.1`). This is what community CI builds
  from today.
- [`description.v0.12.1.draft.yml`](description.v0.12.1.draft.yml) — historical
  draft kept for audit trail only; do not promote it.

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

## Readiness

See [PR_READINESS.md](PR_READINESS.md) for the pre-PR checklist and
[../PRE_COMMUNITY_CHECKLIST.md](../PRE_COMMUNITY_CHECKLIST.md) for the full audit.

Current live ref: **v0.14.1**. Validation covered DuckDB `v1.5.2`, `v1.5.3`,
and `v1.5.4`, plus a DuckDB `v1.5.4` LOAD smoke. New DuckDB releases require
explicit validation.

## Guardrails (C.5)

- No update PR, fork branch, or push to `duckdb/community-extensions` without
  explicit maintainer GO.

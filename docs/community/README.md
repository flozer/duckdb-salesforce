# Community-extensions package (LIVE at v0.9.2 — v0.12.1 update STAGED)

This folder holds the `duckdb/community-extensions` descriptor for
`duckdb-salesforce`.

**The extension is already accepted and live in community at `v0.9.2`** — merged
via [`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037)
(*Add salesforce extension*, MERGED 2026-06-09). Users can install it today:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

A bump to **`v0.12.1`** is **staged as a review draft** and is **GO-gated (gate
C.5)** — no update PR has been opened and the real descriptor is unchanged.
(`v0.12.1` is the clean submission ref; `v0.12.0` was published but its tag
carried stale `vcpkg.json` version metadata, so it is not used as the community
ref — see `docs/RELEASE_NOTES_v0.12.1.md`.)

## Files

- [`description.yml`](description.yml) — the **live** descriptor (`version: 0.9.2`,
  `repo.ref: v0.9.2`). This is what community CI builds from today.
- [`description.v0.12.1.draft.yml`](description.v0.12.1.draft.yml) — the **review
  draft** for the v0.12.1 update (refreshed feature description, `repo.ref:
  v0.12.1`). Not promoted to the real descriptor until C.5 GO.

## What the update PR would contain

A community **update** edits **one file** in a fork of
`duckdb/community-extensions` (the path already exists from #2037):

```
extensions/salesforce/description.yml
```

It would receive the contents of the v0.12.1 draft (after the draft is promoted
to the real `description.yml`). Nothing else changes there — community CI clones
`flozer/duckdb-salesforce` at the descriptor's `repo.ref` and rebuilds + re-signs
from our own `Makefile` / `extension_config.cmake` / `vcpkg.json`.

## Update steps (for when GO is given — NOT done yet)

1. Promote the reviewed draft to the real `description.yml` (`version: 0.12.1`,
   `repo.ref: v0.12.1`) — a GO-gated commit on `flozer`.
2. Fork / update fork of `duckdb/community-extensions`.
3. Edit `extensions/salesforce/description.yml` = the promoted descriptor.
4. Open an **update** PR against `duckdb/community-extensions:main`.
5. Their CI rebuilds + re-signs for each non-excluded platform from `repo.ref`.

## Readiness

See [PR_READINESS.md](PR_READINESS.md) for the pre-PR checklist and
[../PRE_COMMUNITY_CHECKLIST.md](../PRE_COMMUNITY_CHECKLIST.md) for the full audit,
both reframed for the v0.12.1 update.

> **⛔ Currently PARKED** on an upstream, community-wide Windows CI break (DuckDB
> v1.5.3 `fmt` × new MSVC) — **not our code**. Full cause, links, and resume
> criteria: [UPSTREAM_WINDOWS_CI_BLOCKER.md](UPSTREAM_WINDOWS_CI_BLOCKER.md).
> Own-repo latest is **v0.12.1** (clean candidate); community baseline **v0.9.2**.

## Guardrails (C.5)

- No update PR, fork branch, or push to `duckdb/community-extensions` without
  explicit maintainer GO.
- The real `description.yml` stays `v0.9.2`; the v0.12.1 values live in the draft
  until GO.

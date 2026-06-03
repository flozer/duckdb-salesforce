# Community-extensions submission package (STAGED — not submitted)

This folder stages everything needed to submit `duckdb-salesforce` to
[`duckdb/community-extensions`](https://github.com/duckdb/community-extensions).
**Nothing here has been submitted.** Opening the PR is **human-gated (gate
C.5)** and requires the maintainer's explicit GO.

## What the PR would contain

A community submission adds **one file** to a fork of
`duckdb/community-extensions`:

```
extensions/salesforce/description.yml
```

That file is [`description.yml`](description.yml) in this folder, copied verbatim
to the path above. Nothing else is added there — the community CI clones
`flozer/duckdb-salesforce` at `repo.ref` (`v0.8.1`) and builds + signs from our
own `Makefile` / `extension_config.cmake` / `vcpkg.json`.

## Submission steps (for when GO is given — NOT done yet)

1. Fork `duckdb/community-extensions`.
2. Add `extensions/salesforce/description.yml` (= this folder's `description.yml`).
3. Open a PR against `duckdb/community-extensions:main`.
4. Their CI builds + signs for each non-excluded platform from `repo.ref`.

After acceptance, users install with:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

Until then, build + load locally — see [../INSTALL.md](../INSTALL.md).

## Readiness

See [PR_READINESS.md](PR_READINESS.md) for the final pre-PR checklist and
[../PRE_COMMUNITY_CHECKLIST.md](../PRE_COMMUNITY_CHECKLIST.md) for the full audit.

## Guardrails (C.5)

- No PR, branch, fork, or push to `duckdb/community-extensions` without explicit
  maintainer GO.
- Everything here lives in `flozer/duckdb-salesforce` only.

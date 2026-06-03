# Release Notes — v0.8.0

> **STATUS: VALIDATED — tagged `v0.8.0`.** v0.8 is distribution hardening (no new
> features). Acceptance = **CI matrix green**, met (evidence below) and
> authorized by the maintainer. v0.8.0 is local/distribution hardening — it does
> **not** authorize a `duckdb/community-extensions` publication (gate C.5 stays
> active). Local / `flozer`-only.

Commit: `41f9213` (CI + install/pre-community docs).

---

## What's in v0.8 (hardening, not features)

- **CI on the `flozer` repo** (`.github/workflows/MainDistributionPipeline.yml`)
  — GitHub Actions delegating to DuckDB's standard reusable extension pipeline
  (`extension-ci-tools`, pinned `18c54662`). Matrix **DuckDB v1.5.2 + v1.5.3**,
  restricted to **linux_amd64 + windows_amd64**. **Mock-only**: runs the offline
  SQL suite, never sets `SF_LIVE_*`, never contacts Salesforce; `*_live.test`
  skip. Nothing touches `duckdb/community-extensions`.
- **First Linux build** — proven by the Ubuntu CI job (was not provable in the
  maintainer's local box; static portability audit was clean).
- **`docs/INSTALL.md`** — per-OS build, artifact paths, unsigned local `LOAD`
  (`allow_unsigned_extensions`), supported versions + version-locking.
- **`docs/PRE_COMMUNITY_CHECKLIST.md`** — the C.5 gate; does **not** authorize a
  community submission.

No source / behaviour change. Offline suite: 21 `test/sql/*.test` green.

---

## Acceptance evidence — CI matrix GREEN

Run: **Main Distribution Pipeline** @ `41f9213` —
<https://github.com/flozer/duckdb-salesforce/actions/runs/26888031691>

| DuckDB | linux_amd64 | windows_amd64 |
| --- | --- | --- |
| v1.5.2 | ✅ success | ✅ success |
| v1.5.3 | ✅ success | ✅ success |

(Excluded archs — musl / arm64 / osx / wasm / mingw — show as `skipped`, as
intended.) Each cell builds the extension and runs the offline (mock) test
suite. No Salesforce contact, no secrets.

---

## Tag gate — all satisfied

- [x] CI matrix green — Windows + Linux × DuckDB v1.5.2, v1.5.3.
- [x] First Linux build proven (Ubuntu CI).
- [x] Offline suite green (21 files).
- [x] Install + pre-community docs landed.
- [x] Maintainer review of this note.
- [x] **Explicit human GO** to tag `v0.8.0`.

Tag: annotated `v0.8.0` on `flozer/duckdb-salesforce`. A `duckdb/community-extensions`
submission remains human-gated (C.5) — v0.8.0 does not authorize it.

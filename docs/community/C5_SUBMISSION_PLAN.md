# C.5 community UPDATE plan — v0.9.2 → v0.12.1 (PREPARED — NOT EXECUTED)

> **Submission ref: `v0.12.1`.** It supersedes `v0.12.0` as the community ref —
> the `v0.12.0` tag carried a stale `vcpkg.json` `version-string` (`0.11.0`), so
> `v0.12.1` was cut as a clean, consistent provenance tag (no runtime change; see
> `docs/RELEASE_NOTES_v0.12.1.md`). Below, `repo.ref` / `version` are `v0.12.1`;
> `v0.12.0` mentions are auxiliary/historical evidence of the identical code.

> **⛔ PARKED (2026-06-14) — upstream, community-wide CI break.** Do not open the
> update PR yet: `duckdb/community-extensions` Windows CI currently fails for ALL
> extensions — it rebuilds DuckDB v1.5.3 from source on `windows-latest`
> (MSVC 14.51, which removed `stdext::checked_array_iterator`) while DuckDB's
> bundled `fmt` (`format.h:326`) still uses it. Not our code. Refs:
> `duckdb/duckdb#22704`, `duckdb/community-extensions#2061`. Unblocks when DuckDB
> ships the fmt fix in a tag > v1.5.3 (and community bumps its pin) or community
> pins the Windows toolchain; then re-run our matrix at `v0.12.1`, confirm
> Windows green, and proceed. See `PR_READINESS.md`. No local workaround is
> pursued — it cannot fix community's own CI.

> **This is a plan, not an action.** Nothing here has been done for the update.
> No fork branch, no PR, no comment in `duckdb/community-extensions` for v0.12.1.
> Execute only after an explicit maintainer **C.5 GO**, and only after a final
> human confirm of the exact PR contents.
>
> **Context — this is an UPDATE, not a first submission.** The `salesforce`
> extension is already accepted and live in community at **v0.9.2** via
> [`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037)
> (title *Add salesforce extension*, **MERGED 2026-06-09**). This plan bumps that
> already-merged descriptor **0.9.2 → 0.12.1**.

## Current community state

- **Live community baseline:** `salesforce` @ `v0.9.2` (merged via #2037).
  Installable today: `INSTALL salesforce FROM community; LOAD salesforce;`.
- **Proposed update ref:** `v0.12.1` (clean provenance tag at `main`; consistent
  `vcpkg.json` `version-string: 0.12.1`; release assets published, docs EN/PT
  complete). Supersedes `v0.12.0` (stale tag vcpkg metadata).
- **Real descriptor unchanged:** `docs/community/description.yml` still declares
  `version: 0.9.2` / `repo.ref: v0.9.2`. The v0.12.1 values live only in the
  review draft `docs/community/description.v0.12.1.draft.yml` until C.5 GO.

## Preconditions for the update (status)

- [x] `v0.12.0` tagged on `flozer` (`309d9ca`); release assets published
      (linux-x64 + windows-x64), run `27484667321` success.
- [x] Internal version consistency at the v0.12.1 ref: `vcpkg.json`
      `version-string` `0.12.1`, README badge `v0.12.1`.
- [x] Offline mock suite green at v0.12.0: **2540 assertions, 0 fail** (8 live
      tests maintainer-gated/skipped).
- [x] Live maintainer smoke (PII-free) at v0.12.0 — metadata diagnostics +
      `query_cost` + `query_explain` (`docs/smoke/metadata-query-explain-v0.12.0.md`).
- [ ] **Fresh matrix CI green at the `v0.12.1` ref** — Main Distribution Pipeline
      (`linux_amd64`, `windows_amd64`, `osx_arm64` × DuckDB v1.5.2/v1.5.3),
      mock-only, no `SF_LIVE_*`. *(A v0.12.0 run is auxiliary build-proof of the
      identical code; the SUBMISSION-ref proof must be at `v0.12.1`. This CI does
      not publish/submit anything.)*
- [ ] **Public shallow-clone of `v0.12.1`** re-verified (anonymous;
      `vcpkg.json` `version-string: 0.12.1`).
- [ ] **Descriptor draft reviewed** (`description.v0.12.1.draft.yml`).
- [ ] **Maintainer C.5 GO** for the update publication.

## What gets changed (ONLY on GO)

Exactly one file in a fork of `duckdb/community-extensions`, **edited** (the path
already exists from #2037):

- `extensions/salesforce/description.yml` ← contents of
  `docs/community/description.v0.12.1.draft.yml` (after it is promoted to the
  real `docs/community/description.yml`).

No source, no tests, no other docs are copied. Community CI checks out
`flozer/duckdb-salesforce` at `repo.ref: v0.12.1` and rebuilds + re-signs.

## Steps to execute (ONLY on GO — listed, not run)

1. Promote the reviewed draft to the real `docs/community/description.yml`
   (`version: 0.12.1`, `repo.ref: v0.12.1`, updated description) — a separate,
   GO-gated commit on `flozer`.
2. Fork / update fork of `duckdb/community-extensions`.
3. Branch, e.g. `update-salesforce-0.12.1`.
4. Edit `extensions/salesforce/description.yml` = verbatim copy of the promoted
   `docs/community/description.yml`.
5. Commit (`Update salesforce extension to 0.12.1`), push the branch to the fork.
6. Open a PR into `duckdb/community-extensions:main` with the body below.
7. Respond to community-CI / reviewer feedback. Do not merge (maintainers do).

## PR body (draft)

> **Extension:** `salesforce` (update) — read-only Salesforce access as DuckDB
> SQL tables over REST + Bulk. Already in community at `v0.9.2` (#2037); this
> bumps it to `v0.12.1`.
>
> **Repo / ref:** `flozer/duckdb-salesforce` @ `v0.12.1` (annotated tag).
> **License:** MIT. **Dependency:** OpenSSL (via `vcpkg.json`).
> **Platforms:** `linux_amd64`, `windows_amd64` (baseline) + `osx_arm64`
> (extra). Excluded: `osx_amd64`, arm-linux, musl, wasm, mingw, windows_arm64.
>
> **New since v0.9.2:** a shared read-only Metadata Engine with metadata
> diagnostics (`salesforce_metadata_objects`, `salesforce_metadata_fields`); a
> Report Bridge (`salesforce_reports`, `salesforce_report`, describe-validated
> `salesforce_report_soql` with explainability columns); scan explainability
> (`salesforce_query_cost`, `salesforce_query_explain`); and Bulk-backfill
> predicate guardrails. All read-only; no scan-behavior change vs prior diag.
>
> **Evidence:** offline mock suite 2540 assertions green; matrix CI green at
> `v0.12.1` (run TBD — fill at submission); live PII-free smoke validated the
> metadata + explain functions against a real org. Source repo/tag public-clone
> validated.
>
> **Known caveats (declared, unchanged):**
> - macOS live TLS not validated in CI (OpenSSL-via-vcpkg does not read the
>   Keychain); `SSL_CERT_FILE` workaround documented + actionable error hint.
> - JWT bearer offline-covered (real RS256 over a test key + mock token), not
>   live-validated against a pre-authorized Connected App.
> - Blob/base64 body fields not byte-readable (documented, guarded, by design).
> - CI runner emits Node.js action-deprecation warnings (non-blocking).

## Caveats to declare (summary)

macOS live TLS (workaround documented) · JWT not live-validated · blob bodies
not byte-readable (by design) · Node action warnings · excluded platforms.

## Guardrails

No PR is opened, no fork branch is pushed, and the real
`docs/community/description.yml` is not changed until the maintainer says GO and
confirms this exact plan. The v0.12.1 values live in the draft file only.

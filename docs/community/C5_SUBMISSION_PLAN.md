# C.5 community submission plan (PREPARED — NOT EXECUTED)

> **This is a plan, not an action.** Nothing here has been done. No fork, no
> branch, no PR, no comment in `duckdb/community-extensions`. Execute only after
> an explicit maintainer **C.5 GO**, and only after a final human confirm of the
> exact PR contents. Submission ref: **`v0.9.2`**.

## Preconditions

- [x] `v0.9.2` tagged on `flozer`, tree self-consistent (descriptor `repo.ref`
      `v0.9.2`, `vcpkg.json` version-string `0.9.2`). v0.9.2 is an operational
      distribution release — functionally identical to `v0.9.1` (no connector
      code change); it only adds the release-assets workflow + packaging.
- [x] Offline suite green at the ref (34 files / 921 assertions).
- [x] Remote CI green at the submission ref — run **27136017797** at `v0.9.2`,
      all 6 platform×version jobs (`linux_amd64`, `windows_amd64`,
      `osx_arm64` × DuckDB `v1.5.2`, `v1.5.3`), mock-only, no `SF_LIVE_*`.
      Earlier same-matrix run **27026331956** also passed before the operational
      `v0.9.2` packaging tag.
- [x] Light live smoke validated at `v0.9.1` (see `docs/RELEASE_NOTES_v0.9.1.md`);
      applies unchanged to `v0.9.2`.
- [x] Release assets published for `v0.9.2` via `release-assets.yml` (Linux x64
      tar.gz + Windows x64 zip) — see `docs/RELEASE_NOTES_v0.9.2.md`.
- [x] **Repository visibility:** `flozer/duckdb-salesforce` is public. Firebird
      failed community CI once because the source repo was private and `repo.ref`
      could not be cloned; this preflight is now closed here.
- [x] **Public clone/tag preflight:** verified:

      ```sh
      git -c credential.helper= ls-remote --tags https://github.com/flozer/duckdb-salesforce.git v0.9.2
      git clone --depth=1 --branch v0.9.2 https://github.com/flozer/duckdb-salesforce.git
      ```

      The anonymous shallow clone resolves to commit `9c58fb1` at tag `v0.9.2`.

- [ ] **Maintainer C.5 GO**.

## What gets copied

Exactly one file, unchanged, into a fork of `duckdb/community-extensions`:

- `docs/community/description.yml`  →  `extensions/salesforce/description.yml`

No source, no tests, no other docs are copied. The community CI checks out
`flozer/duckdb-salesforce` at `repo.ref: v0.9.2` and builds + signs from there.

## Steps to execute (ONLY on GO — listed, not run)

1. Final-review `docs/community/description.yml` exactly as it will be copied.
2. Fork `duckdb/community-extensions` (or use an existing fork) under the
   maintainer's GitHub account.
3. Branch, e.g. `add-salesforce`.
4. Add `extensions/salesforce/description.yml` = a verbatim copy of
   `docs/community/description.yml` from `flozer` at `v0.9.2`.
5. Commit (`Add salesforce extension`), push the branch to the fork.
6. Open a PR into `duckdb/community-extensions:main` with the body below.
7. Respond to community-CI / reviewer feedback. Do not merge (maintainers do).

## PR body (draft)

> **Extension:** `salesforce` — read-only access to Salesforce orgs as DuckDB
> SQL tables over the official REST and Bulk APIs.
>
> **Repo / ref:** `flozer/duckdb-salesforce` @ `v0.9.2` (annotated tag).
> **License:** MIT. **Dependency:** OpenSSL (via `vcpkg.json`).
> **Platforms:** `linux_amd64`, `windows_amd64` (baseline) + `osx_arm64`
> (extra). Excluded: `osx_amd64`, arm-linux, musl, wasm, mingw, windows_arm64.
>
> **What it does:** native `ATTACH` of an org as a read-only catalog; REST
> `/query` + `queryAll`, Bulk API 2.0 (lazy streaming, PK chunking, auto
> transport); projection/predicate/`COUNT(*)` pushdown; explicit
> `salesforce_aggregate()` with `GROUP BY`; parent/grandparent relationship
> STRUCTs; metadata helpers (`salesforce_refresh_metadata`,
> `salesforce_picklist_values`, `salesforce_record_types`); diagnostics. OAuth
> refresh-token or JWT bearer; credentials from options / env / SFDX URL;
> read-only (all writes throw); TLS verification always on.
>
> **Evidence:** offline mock suite 34 files / 921 assertions green; CI run
> 27136017797 green on all 6 platform×version jobs at `v0.9.2`; a light live smoke against a
> real org validated the metadata functions + REST/Bulk scan consistency. The
> source repo/tag is public-clone validated.
>
> **Known caveats (declared):**
> - macOS live TLS not validated in CI (OpenSSL-via-vcpkg does not read the
>   Keychain); `SSL_CERT_FILE` workaround documented + an actionable error hint.
>   A zero-config Keychain trust store is a planned follow-up.
> - JWT bearer is offline-covered (real RS256 over a test key + mock token) but
>   not live-validated against a pre-authorized Connected App.
> - Blob/base64 body fields are not byte-readable (Bulk CSV rejects them; REST
>   returns a URL reference) — documented, guarded, by design.
> - CI runner emits Node.js 20 action-deprecation warnings (non-blocking).

## Caveats to declare (summary)

macOS live TLS (workaround documented) · JWT not live-validated · blob bodies
not byte-readable (by design) · Node20 action warnings · excluded platforms.

## Guardrails

No PR is opened, no fork created, and nothing is pushed to any
`duckdb/community-extensions`-related repo until the maintainer says GO and
confirms this exact plan. This file lives in `flozer` only.

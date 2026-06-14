# Community UPDATE PR readiness — v0.9.2 → v0.12.0

Final gate before opening a `duckdb/community-extensions` **update** PR. **No PR
and no real-descriptor change until the maintainer's explicit GO (C.5).**

> **This is an update, not a first submission.** `salesforce` is already live in
> community at **v0.9.2** (merged via
> [`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037),
> *Add salesforce extension*, **MERGED 2026-06-09**). This checklist tracks the
> bump to **v0.12.0**. The v0.12.0 descriptor values live in the review draft
> `docs/community/description.v0.12.0.draft.yml`; the real
> `docs/community/description.yml` stays `0.9.2` until C.5 GO.

## Descriptor draft (`description.v0.12.0.draft.yml`)

- [x] `extension.name: salesforce` (matches the merged extension).
- [x] `description` — refreshed to v0.12.0 surface: Report Bridge, Metadata
      Engine v2 + diagnostics (`salesforce_metadata_objects` /
      `salesforce_metadata_fields`), scan explainability (`salesforce_query_cost`
      / `salesforce_query_explain`), Bulk-backfill guardrails — on top of the
      v0.9.x feature set.
- [x] `version: 0.12.0` — matches the proposed update tag.
- [x] `language: C++`, `build: cmake`, `license: MIT`, `maintainers: [flozer]`.
- [x] `excluded_platforms` unchanged (baseline linux_amd64 + windows_amd64 +
      extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.12.0`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [ ] YAML re-validated (parse) before promotion to the real descriptor.

## Repo / build

- [x] `v0.12.0` tag exists (`309d9ca`) — the intended update commit; release
      assets published (run `27484667321` success).
- [x] Internal version consistency: `vcpkg.json` `version-string` `0.12.0`,
      README badge `v0.12.0` (commit `525188f`).
- [x] Offline suite green at the ref: **2540 assertions, 0 fail** (local).
- [ ] **Fresh matrix CI green at `v0.12.0`** — Main Distribution Pipeline
      (`linux_amd64`, `windows_amd64`, `osx_arm64` × DuckDB v1.5.2/v1.5.3),
      mock-only, no `SF_LIVE_*`. *(Last green matrix run `27136017797` was at
      `v0.9.2`; a v0.12.0 run is REQUIRED. It does not publish/submit anything.)*
- [x] `vcpkg.json` declares the only dependency (OpenSSL); community CI resolves it.
- [x] `Makefile` + `extension_config.cmake` drive the standard build.
- [x] Submodules (`duckdb`, `extension-ci-tools`) pinned.
- [ ] **Local builds re-validated at v0.12.0** — MSVC (VS BuildTools cmake/ninja,
      vcpkg `x64-windows-static` OpenSSL) primary; RTOOLS/MinGW
      (`x64-mingw-static` OpenSSL 3) via `scripts/build_rtools_local.ps1`.
- [ ] **LOAD test of the published v0.12.0 artifact** — `salesforce.duckdb_extension`
      from the release archive loads + the new functions resolve.
- [ ] **Anonymous shallow clone of `v0.12.0`** re-verified.
- [x] **Release assets published for `v0.12.0`** (linux-x64 tar.gz + windows-x64
      zip) — see `docs/RELEASE_NOTES_v0.12.0.md`.

## Legal / security / docs

- [x] LICENSE (MIT), SECURITY.md, THIRD_PARTY_NOTICES.md present.
- [x] CONTRIBUTING.md + CODE_OF_CONDUCT.md present.
- [x] README + bilingual docs (usage guide, function manual) refreshed for the
      v0.12.0 surface (EN/PT parity, `docs/DOCS_PARITY.md`).
- [x] No secrets, tokens, org identifiers, or customer data in repo/CI.
- [x] CI never contacts Salesforce / never needs `SF_LIVE_*`.

## Known, accepted caveats (non-blockers, unchanged)

- [x] Live Salesforce TLS on **macOS** not validated in CI (Keychain not read by
      OpenSSL-via-vcpkg); `SSL_CERT_FILE` workaround documented + actionable
      error hint. Baseline parity is met by linux_amd64 + windows_amd64.
- [x] **JWT bearer not live-validated** — offline-covered (real RS256 over a test
      key + mock token); no live run against a pre-authorized Connected App.
- [x] `osx_amd64`, arm-linux, musl, wasm, mingw out of scope (excluded).

## Final gate

- [ ] **Maintainer explicit GO** to promote the draft + open the
      community-extensions UPDATE PR (C.5).

Until that box is checked by a human, the real descriptor stays `0.9.2` and the
v0.12.0 values live only in the draft.

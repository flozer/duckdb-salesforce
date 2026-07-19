# Community UPDATE PR readiness — v0.9.2 → v0.12.1

> **Superseded (2026-06-19):** community update `v0.14.1` was merged in
> [`duckdb/community-extensions#2078`](https://github.com/duckdb/community-extensions/pull/2078).
> This document is retained as historical planning evidence for the earlier
> `v0.12.1` attempt and upstream Windows CI blocker.

Final gate before opening a `duckdb/community-extensions` **update** PR. **No PR
and no real-descriptor change until the maintainer's explicit GO (C.5).**

> ## ⛔ PARKED — blocked by an UPSTREAM, community-wide CI break (not our code)
>
> As of **2026-06-14**, `duckdb/community-extensions` Windows CI fails for **all**
> extensions. It rebuilds **DuckDB v1.5.3 from source** on `windows-latest`
> (now windows-2025 / MSVC Build Tools **14.51**), which **removed
> `stdext::checked_array_iterator`**, while DuckDB v1.5.3's bundled
> `third_party/fmt/include/fmt/format.h:326` still references it (mis-guarded
> `#ifdef _SECURE_SCL`). Compile dies in DuckDB before our extension is touched.
>
> - DuckDB `main` already removed the offending code, **but no tag > v1.5.3
>   exists yet**.
> - Refs: DuckDB issue `duckdb/duckdb#22704`; community issue
>   `duckdb/community-extensions#2061`.
> - **Our side is clean:** extension code compiles on new MSVC; published
>   v0.12.1 assets build (release-assets pins `windows-2022`) + LOAD; full mock
>   suite 2540/0; linux + osx_arm64 CI green; RTools/MinGW green.
>
> **Unblock criteria (any one):** DuckDB publishes v1.5.4 / v1.6.0 with the fmt
> fix and community bumps its pin; **or** community points its DuckDB ref at a
> fixed `main` commit; **or** community pins the Windows runner/toolset.
> **Then:** re-dispatch our matrix at `v0.12.1`, confirm Windows green, and only
> then proceed to the C.5 update PR. No local workaround (`/U_SECURE_SCL`,
> vendored fmt patch, runner pin) is pursued — none would fix community's own CI.

> **Submission ref: `v0.12.1`** (supersedes `v0.12.0`, whose tag carried stale
> `vcpkg.json` version metadata — see `docs/RELEASE_NOTES_v0.12.1.md`; no runtime
> change). `v0.12.0` mentions below are auxiliary/historical evidence of the
> identical code.
>
> **This is an update, not a first submission.** `salesforce` is already live in
> community at **v0.9.2** (merged via
> [`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037),
> *Add salesforce extension*, **MERGED 2026-06-09**). This checklist tracks the
> bump to **v0.12.1**. The v0.12.1 descriptor values live in the review draft
> `docs/community/description.v0.12.1.draft.yml`; the real
> `docs/community/description.yml` stays `0.9.2` until C.5 GO.

## Descriptor draft (`description.v0.12.1.draft.yml`)

- [x] `extension.name: salesforce` (matches the merged extension).
- [x] `description` — refreshed to v0.12.1 surface: Report Bridge, Metadata
      Engine v2 + diagnostics (`salesforce_metadata_objects` /
      `salesforce_metadata_fields`), scan explainability (`salesforce_query_cost`
      / `salesforce_query_explain`), Bulk-backfill guardrails — on top of the
      v0.9.x feature set.
- [x] `version: 0.12.1` — matches the proposed update tag.
- [x] `language: C++`, `build: cmake`, `license: MIT`, `maintainers: [flozer]`.
- [x] `excluded_platforms` unchanged (baseline linux_amd64 + windows_amd64 +
      extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.12.1`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [ ] YAML re-validated (parse) before promotion to the real descriptor.

## Repo / build

- [x] `v0.12.1` tag exists (`b5e769d`) — clean provenance tag at `main`; release
      assets published via `release-assets.yml` (linux-x64 + windows-x64 both
      green; windows built on pinned `windows-2022`). *(v0.12.0 tag `309d9ca` +
      its assets are auxiliary — identical code, stale vcpkg metadata.)*
- [x] Internal version consistency at the v0.12.1 ref: `vcpkg.json`
      `version-string` `0.12.1`, README badge `v0.12.1`.
- [x] Offline suite green at the ref: **2540 assertions, 0 fail** (local).
- [x] Anonymous shallow clone of `v0.12.1` resolves `b5e769d`; `vcpkg` `0.12.1`;
      draft descriptor `version`/`ref` `0.12.1`.
- [x] LOAD of the published v0.12.1 windows-x64 artifact into stock DuckDB
      v1.5.3 — key functions resolve (`salesforce_query_explain`,
      `salesforce_metadata_objects`/`_fields`, `salesforce_report_soql`).
- [x] RTools/MinGW build green — links `salesforce.duckdb_extension` + unittest;
      direct binary run **2540/0/8**. (The build-script `fail=1` is a harness
      false-negative on `..._noscan.test`, which itself passes;
      `windows_amd64_mingw` is an excluded community platform — secondary proof.)
- [ ] ⛔ **Fresh matrix CI green at `v0.12.1`** — **BLOCKED upstream** (see banner):
      `linux_amd64` + `osx_arm64` green; **`windows_amd64` fails in DuckDB's own
      `fmt` build** (community-wide; `duckdb#22704` / `community-extensions#2061`).
      Unblocks when DuckDB > v1.5.3 lands or community pins the toolchain.
- [x] `vcpkg.json` declares the only dependency (OpenSSL); community CI resolves it.
- [x] `Makefile` + `extension_config.cmake` drive the standard build.
- [x] Submodules (`duckdb`, `extension-ci-tools`) pinned.
- [ ] **Local builds re-validated at v0.12.1** — MSVC (VS BuildTools cmake/ninja,
      vcpkg `x64-windows-static` OpenSSL) primary; RTOOLS/MinGW
      (`x64-mingw-static` OpenSSL 3) via `scripts/build_rtools_local.ps1`.
- [ ] **LOAD test of the published v0.12.1 artifact** — `salesforce.duckdb_extension`
      from the release archive loads + the new functions resolve.
- [ ] **Anonymous shallow clone of `v0.12.1`** re-verified.
- [ ] **Release assets published for `v0.12.1`** (linux-x64 tar.gz + windows-x64
      zip) — produced by `release-assets.yml` on the `v0.12.1` tag push; see
      `docs/RELEASE_NOTES_v0.12.1.md`.

## Legal / security / docs

- [x] LICENSE (MIT), SECURITY.md, THIRD_PARTY_NOTICES.md present.
- [x] CONTRIBUTING.md + CODE_OF_CONDUCT.md present.
- [x] README + bilingual docs (usage guide, function manual) refreshed for the
      v0.12.1 surface (EN/PT parity, `docs/DOCS_PARITY.md`).
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
v0.12.1 values live only in the draft.

# Community PR readiness checklist

Final gate before opening a `duckdb/community-extensions` PR. **No PR until the
maintainer's explicit GO (C.5).**

## Descriptor (`description.yml`)

- [x] `extension.name: salesforce` (unique; not taken in community-extensions).
- [x] `description` — concise, accurate, read-only scope stated (covers through
      v0.9.1: queryAll, salesforce_aggregate + GROUP BY, grandparent relationships
      + diagnostics, refresh-token/JWT auth, options/env/SFDX sources, metadata
      helpers — refresh / picklist values / record types, Bulk blob/base64 guard).
- [x] `version: 0.9.2` — matches the submission tag.
- [x] `language: C++`, `build: cmake`.
- [x] `license: MIT` (LICENSE present at repo root).
- [x] `maintainers: [flozer]`.
- [x] `excluded_platforms` reflects policy (baseline linux_amd64 + windows_amd64
      + extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.9.2`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [x] YAML parses; required keys present (validated locally).

## Repo / build

- [ ] Public repo + public tag clone validated. **Current blocker:** as of the
      Firebird-parity preflight, `flozer/duckdb-salesforce` is still private.
      Before any community PR, make it public and verify:
      `git -c credential.helper= ls-remote --tags
      https://github.com/flozer/duckdb-salesforce.git v0.9.2`.
- [x] `v0.9.2` tag exists and is the intended submission commit — an operational
      distribution release, functionally identical to `v0.9.1` (no connector code
      change; adds the release-assets workflow + packaging). `v0.9.1` carried the
      light live smoke (docs/RELEASE_NOTES_v0.9.1.md), which applies unchanged.
- [x] Offline suite green at the ref: 34 files / 921 assertions (local).
- [x] Remote CI green at `v0.9.1`: run **27026331956** passed all 6
      platform/version jobs (`linux_amd64`, `windows_amd64`, `osx_arm64` ×
      DuckDB `v1.5.2`, `v1.5.3`). Mock-only; no `SF_LIVE_*`; no Salesforce
      contact.
- [x] `vcpkg.json` declares the only dependency (OpenSSL); community CI resolves it.
- [x] `Makefile` + `extension_config.cmake` drive the standard build.
- [x] Submodules (`duckdb`, `extension-ci-tools`) pinned.
- [x] Local builds validated: **MSVC** (VS BuildTools — cmake/ninja, vcpkg
      `x64-windows-static` OpenSSL) is the primary maintainer build; **RTOOLS /
      MinGW** (`x64-mingw-static` OpenSSL 3) via `scripts/build_rtools_local.ps1`
      also builds. Linux/Windows/macOS-arm64 are covered by the canonical CI
      matrix; community CI builds from `vcpkg.json` regardless.
- [x] **Release assets published for `v0.9.2`** — the `v0.9.2` tag push triggered
      `release-assets.yml` run **27035482676** (Linux + Windows jobs green), which
      created the GitHub Release and uploaded both binaries. Release:
      <https://github.com/flozer/duckdb-salesforce/releases/tag/v0.9.2>. Assets,
      verified downloadable and coherent (each archive holds
      `salesforce.duckdb_extension` + a `README.txt` stamped `0.9.2`, requiring
      DuckDB v1.5.3):
      - `duckdb-salesforce-0.9.2-linux-x64.tar.gz` (~11.6 MB)
      - `duckdb-salesforce-0.9.2-windows-x64.zip` (~10.1 MB)
      The Release changelog is sourced from `docs/RELEASE_NOTES_v0.9.2.md`.
      (Prior manual validation run on `main` was **27031409510**.)

## Legal / security / docs

- [x] LICENSE (MIT), SECURITY.md, THIRD_PARTY_NOTICES.md present.
- [x] CONTRIBUTING.md + CODE_OF_CONDUCT.md present.
- [x] README + bilingual docs (usage guide, function manual) present.
- [x] No secrets, tokens, org identifiers, or customer data in repo/CI.
- [x] CI never contacts Salesforce / never needs `SF_LIVE_*`.

## Known, accepted caveats (non-blockers)

- [x] Live Salesforce TLS on **macOS** not validated (Keychain not read by
      OpenSSL-via-vcpkg); `SSL_CERT_FILE` workaround documented + an actionable
      error hint added. CI proves macOS build + offline tests only. The v0.9.0
      live smoke ran on a non-macOS host, so macOS live TLS is still un-smoked.
      Zero-config Keychain trust store (Security.framework) is a queued follow-up.
- [x] **JWT bearer (`auth_source 'jwt'`) not live-validated** — the v0.9.0 smoke
      used refresh-token (`auth_source 'env'`); JWT signing/exchange is offline-
      covered (real RS256 over a test key + mock token), but no live run against
      a pre-authorized Connected App has been done.
- [x] `osx_amd64`, arm-linux, musl, wasm, mingw out of scope (excluded).
- [x] `vcpkg.json` `version-string` is `0.9.2` (matches the tag / descriptor).

## Final gate

- [ ] **Maintainer explicit GO** to open the community-extensions PR (C.5).

Before opening the PR, repeat the public-clone check above. This is not a formality:
the Firebird community PR initially failed because the source repository was
private, so community CI could not clone the pinned `repo.ref`.

Until that box is checked by a human, the extension stays local / `flozer`-only.

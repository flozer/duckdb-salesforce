# Community PR readiness checklist

Final gate before opening a `duckdb/community-extensions` PR. **No PR until the
maintainer's explicit GO (C.5).**

## Descriptor (`description.yml`)

- [x] `extension.name: salesforce` (unique; not taken in community-extensions).
- [x] `description` — concise, accurate, read-only scope stated (covers through
      v0.9.1: queryAll, salesforce_aggregate + GROUP BY, grandparent relationships
      + diagnostics, refresh-token/JWT auth, options/env/SFDX sources, metadata
      helpers — refresh / picklist values / record types, Bulk blob/base64 guard).
- [x] `version: 0.9.1` — matches the submission tag.
- [x] `language: C++`, `build: cmake`.
- [x] `license: MIT` (LICENSE present at repo root).
- [x] `maintainers: [flozer]`.
- [x] `excluded_platforms` reflects policy (baseline linux_amd64 + windows_amd64
      + extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.9.1`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [x] YAML parses; required keys present (validated locally).

## Repo / build

- [ ] Public repo + public tag clone validated. **Current blocker:** as of the
      Firebird-parity preflight, `flozer/duckdb-salesforce` is still private.
      Before any community PR, make it public and verify:
      `git -c credential.helper= ls-remote --tags
      https://github.com/flozer/duckdb-salesforce.git v0.9.1`.
- [x] `v0.9.1` tag exists and is the intended submission commit (validated by a
      light live smoke — see docs/RELEASE_NOTES_v0.9.1.md). `v0.9.0` was the prior
      candidate (docs/RELEASE_NOTES_v0.9.md).
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
- [x] Release-asset workflow green on `main`: run **27031409510** produced
      Linux x64 and Windows x64 workflow artifacts. No GitHub Release was created
      because the run was manual without a release tag input. Note: the existing
      `v0.9.1` tag predates this workflow; tagged binary releases should use a
      later tag that contains `.github/workflows/release-assets.yml`.

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
- [x] `vcpkg.json` `version-string` is `0.9.1` (matches the tag / descriptor).

## Final gate

- [ ] **Maintainer explicit GO** to open the community-extensions PR (C.5).

Before opening the PR, repeat the public-clone check above. This is not a formality:
the Firebird community PR initially failed because the source repository was
private, so community CI could not clone the pinned `repo.ref`.

Until that box is checked by a human, the extension stays local / `flozer`-only.

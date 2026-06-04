# Community PR readiness checklist

Final gate before opening a `duckdb/community-extensions` PR. **No PR until the
maintainer's explicit GO (C.5).**

## Descriptor (`description.yml`)

- [x] `extension.name: salesforce` (unique; not taken in community-extensions).
- [x] `description` — concise, accurate, read-only scope stated (covers v0.9.0:
      queryAll, salesforce_aggregate + GROUP BY, grandparent relationships +
      diagnostics, refresh-token/JWT auth, options/env/SFDX sources).
- [x] `version: 0.9.0` — matches the submission tag.
- [x] `language: C++`, `build: cmake`.
- [x] `license: MIT` (LICENSE present at repo root).
- [x] `maintainers: [flozer]`.
- [x] `excluded_platforms` reflects policy (baseline linux_amd64 + windows_amd64
      + extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.9.0`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [x] YAML parses; required keys present (validated locally).

## Repo / build

- [x] Public repo, `main` clean.
- [x] `v0.9.0` tag exists and is the intended commit (validated by the live
      smoke — see docs/RELEASE_NOTES_v0.9.md).
- [x] Offline suite green at the ref: 28 files / 777 assertions (local).
- [x] Remote CI green at `v0.9.0`: linux_amd64 + windows_amd64 + osx_arm64 ×
      DuckDB v1.5.2/v1.5.3 (offline mock suite, no `SF_LIVE_*`). Remote CI is
      manual-only (`workflow_dispatch`); a one-time run at the submission ref was
      triggered for the package. Evidence: run **26972597225**, conclusion
      **success**, all 6 platform×version jobs green
      ([run 26972597225](https://github.com/flozer/duckdb-salesforce/actions/runs/26972597225)).
      Note: runner emitted Node.js 20 action-deprecation warnings (non-blocking;
      bump action versions at a future maintenance pass).
- [x] `vcpkg.json` declares the only dependency (OpenSSL); community CI resolves it.
- [x] `Makefile` + `extension_config.cmake` drive the standard build.
- [x] Submodules (`duckdb`, `extension-ci-tools`) pinned.

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
- [x] `vcpkg.json` `version-string` bumped to `0.9.0` (matches the tag /
      descriptor). The earlier `0.8.0` cosmetic gap is closed.

## Final gate

- [ ] **Maintainer explicit GO** to open the community-extensions PR (C.5).

Until that box is checked by a human, the extension stays local / `flozer`-only.

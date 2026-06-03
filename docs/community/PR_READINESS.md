# Community PR readiness checklist

Final gate before opening a `duckdb/community-extensions` PR. **No PR until the
maintainer's explicit GO (C.5).**

## Descriptor (`description.yml`)

- [x] `extension.name: salesforce` (unique; not taken in community-extensions).
- [x] `description` — concise, accurate, read-only scope stated.
- [x] `version: 0.8.1` — matches the submission tag.
- [x] `language: C++`, `build: cmake`.
- [x] `license: MIT` (LICENSE present at repo root).
- [x] `maintainers: [flozer]`.
- [x] `excluded_platforms` reflects policy (baseline linux_amd64 + windows_amd64
      + extra osx_arm64; osx_amd64/arm-linux/musl/wasm/mingw excluded).
- [x] `repo.github: flozer/duckdb-salesforce`, `repo.ref: v0.8.1`.
- [x] `docs.hello_world` uses the signed path (`INSTALL ... FROM community`).
- [x] YAML parses; required keys present (validated locally).

## Repo / build

- [x] Public repo, `main` clean.
- [x] `v0.8.1` tag exists and is the intended commit (includes macOS CI + docs).
- [x] CI green at that ref: linux_amd64 + windows_amd64 + osx_arm64 ×
      DuckDB v1.5.2/v1.5.3 (offline mock suite).
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
      OpenSSL-via-vcpkg); `SSL_CERT_FILE` workaround documented. CI proves
      macOS build + offline tests only.
- [x] `osx_amd64`, arm-linux, musl, wasm, mingw out of scope (excluded).

## Final gate

- [ ] **Maintainer explicit GO** to open the community-extensions PR (C.5).

Until that box is checked by a human, the extension stays local / `flozer`-only.

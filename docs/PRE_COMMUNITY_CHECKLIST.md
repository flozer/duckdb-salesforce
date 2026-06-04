# Pre-Community Submission Checklist

> **This document does NOT authorize a community submission.** Per gate **C.5**,
> publishing to `duckdb/community-extensions` (PR / push / release there) happens
> **only after explicit human approval** by the maintainer, once everything
> below is true. Nothing in this repo's automation touches
> `duckdb/community-extensions`.

The goal: before a human decides to submit, the connector must be **repeatable,
auditable, and distributable**. Tick every box first.

## Build & test

- [ ] Offline (mock) test suite green on **all** `test/sql/*.test`.
- [ ] CI green on the platform matrix: **Linux + Windows (required baseline) +
      macOS arm64 (extra) × DuckDB v1.5.2, v1.5.3**. Salesforce thus covers the
      duckdb-firebird platform parity (linux_amd64 + windows_amd64) **plus**
      `osx_arm64`. `osx_amd64`/arm-linux/musl/wasm/mingw excluded for now.
- [ ] First **Linux** build proven (Ubuntu CI job) — was not provable in the
      maintainer's local box; CI is the proof.
- [ ] Clean build from a fresh `git submodule update --init --recursive`.
- [ ] No compiler warnings treated as errors / no portability `#ifdef` gaps.

## Live validation (maintainer-only, never in CI)

- [ ] Manual live smoke against a maintainer-authorized org for the release's
      features (REST, Bulk, auto, quota, COUNT, relationships, Tooling, PK
      chunking) — recorded **secret-free** in the matching `RELEASE_NOTES_*`.
- [ ] CI **never** contacts Salesforce and **never** requires secrets
      (no `SF_LIVE_*`); `*_live.test` skip without a live org.

## Packaging / release review

- [ ] Loadable artifact builds per platform: `salesforce.duckdb_extension`.
- [ ] Version stamped (`EXT_VERSION` / tag) and matches the release notes.
- [ ] Local install documented ([INSTALL.md](INSTALL.md)) incl. unsigned
      `LOAD` (`allow_unsigned_extensions`).
- [ ] Supported DuckDB range documented; extension version-locking explained.

## Metadata & legal (required by community-extensions)

- [x] `description.yml` drafted (`docs/community/description.yml`) — name,
      description, version, maintainers, repo, license, language. (Staged, not
      submitted.)
- [x] LICENSE present and compatible (MIT).
- [x] `SECURITY.md` present (vuln reporting, TLS-on, no-secret-logging).
- [x] No secrets, tokens, org identifiers, or customer data in the repo/CI.
- [x] README documents auth (refresh-token OAuth), read-only scope, limitations.
- [x] CONTRIBUTING.md + CODE_OF_CONDUCT.md + THIRD_PARTY_NOTICES.md present.
- [x] Public docs (EN primary, PT) — usage guide + function manual paired;
      Windows/Linux build guides; parity tracked in `docs/DOCS_PARITY.md`.

## Behaviour / safety

- [x] Read-only: all mutating catalog ops throw.
- [x] Errors are secret-free (no bearer/body/secret in messages).
- [x] Quota governor + REQUEST_LIMIT_EXCEEDED handling documented.
- [x] Bulk CSV decode hardened + REST/Bulk type parity tested (v0.9 §2).

### Known behaviour / future hardening (non-blockers)

- `sfcsv::Parse` is **lenient on a malformed unterminated quote** — it flushes
  the trailing field instead of erroring. Real Salesforce Bulk CSV is
  well-formed, so this is accepted as-is; adding strict detection is a future
  hardening item, not a blocker (clear errors already fire on invalid typed
  conversion).

## Sign-off

- [ ] Maintainer reviews this checklist and the latest release notes.
- [ ] **Explicit human GO** recorded to submit to `duckdb/community-extensions`.

Until the final box is checked by a human, the extension stays **local /
`flozer`-only**.

---

## Audit results (v0.8 community-prep, flozer-only — no submission)

Done as a local audit. **No PR/push/branch on `duckdb/community-extensions`.**

### Pass

- ✅ **No secrets** in tracked files (scan clean; `.gitignore` covers
  `.env`/`secrets.*`). CI is mock-only, no `SF_LIVE_*`.
- ✅ **CI green** — Win + Linux × DuckDB v1.5.2/v1.5.3 (run 26888031691).
- ✅ **SECURITY.md** present; refreshed to v0.8 status.
- ✅ **README** present; v0.1 status banner refreshed to v0.8.
- ✅ **Vendored license** — `third_party/httplib` is MIT (Yuji Hirose);
  OpenSSL via vcpkg (Apache-2.0). Compatible.
- ✅ **Version stamps** aligned to 0.8.0 (`vcpkg.json`).
- ✅ **Read-only** + secret-free errors + TLS-on verified earlier.
- ✅ **description.yml** drafted (staged at
  [`community/description.yml`](community/description.yml)) — NOT
  submitted.

### Gaps — status

1. ✅ **`LICENSE`** — **MIT** added at repo root (© 2026 Fernando Lozer);
   `description.yml` draft set to `license: MIT`. (Was the hard blocker.)
2. ✅ **THIRD-PARTY NOTICES** — [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md):
   httplib (MIT), OpenSSL (Apache-2.0), DuckDB/extension-ci-tools/vcpkg (MIT).
3. ✅ **Stale-string sweep** — README + SECURITY refreshed to v0.8; `vcpkg.json`
   0.8.0; `docs/ARCHITECTURE.md` Appendix C marked HISTORICAL + `repo.ref`
   example updated. (Appendix C kept verbatim for provenance.)
4. ✅ **Platform coverage** — CI-validated: **`linux_amd64` + `windows_amd64` +
   `osx_arm64`** (build + offline mock suite, all green on DuckDB v1.5.2/v1.5.3).
   `osx_amd64` (cross-compile), arm-linux, musl, wasm, mingw remain excluded —
   a documented, conscious scope choice. **CI proves the macOS build + offline
   tests, NOT live Salesforce TLS on macOS** (see #5).
5. ⚠️ **Live-TLS on macOS — known gap, mitigated (option A shipped).**
   Maintainer decision: ship the `SSL_CERT_FILE` path now (option A); defer the
   zero-config Keychain trust store (option B) to a follow-up. Status:
   - macOS build + offline (mock) tests: **green** (osx_arm64, v1.5.2/v1.5.3).
   - live Salesforce TLS on macOS: **not validated in CI** (mock-only); the
     supported path is `SSL_CERT_FILE` pointing at a CA bundle.
   - **A (done):** a macOS TLS-verify failure now prints an actionable hint
     (`export SSL_CERT_FILE=$(brew --prefix)/etc/openssl@3/cert.pem` or certifi);
     documented in INSTALL.md, SECURITY.md, and the EN/PT usage guides.
     Verification stays on — this selects trust anchors, it is not a bypass.
   - **not a blocker** for package / community readiness (CI is mock-only;
     community parity is met by the linux_amd64 + windows_amd64 baseline).
   - **B (follow-up, optional):** load the macOS system trust store via
     `Security.framework` (`SecTrustCopyAnchorCertificates`), mirroring the
     Windows `wincrypt` path, for zero-config live use. Implement only when a
     macOS compile/test cycle is available.
6. ℹ️ **Signing** — community extensions are signed by DuckDB's pipeline; local
   builds stay unsigned (documented in INSTALL.md). No action until submission.
7. ℹ️ **Submission mechanics** — a PR to `duckdb/community-extensions` adds
   `extensions/salesforce/description.yml` pointing at a tagged ref. Human-gated
   (C.5); not prepared as a branch/PR anywhere.

**Net:** mechanical + platform gaps closed (CI green on Linux/Windows/macOS).
Package is **submission-ready** except the documented live-macOS-TLS gap (#5,
not a parity blocker) and the **explicit human GO** (C.5) — which remains open
by design. No submission performed.

# C.6 community UPDATE plan — v0.14.1 → v0.15.0 (PREPARED — NOT EXECUTED)

> **This is a plan, not an action.** Nothing here has been done for the
> update. No fork branch, no PR, no comment in `duckdb/community-extensions`
> for v0.15.0. Execute only after an explicit maintainer **C.5 GO** (same gate
> used for the v0.9.2→v0.12.1 and v0.12.1→v0.14.1 rounds — see the superseded
> `C5_SUBMISSION_PLAN.md` / `PR_READINESS.md`), and only after a final human
> confirm of the exact PR contents.
>
> **Context — this is an UPDATE, not a first submission.** `salesforce` is
> already accepted and live in community at **v0.14.1** via
> [`duckdb/community-extensions#2078`](https://github.com/duckdb/community-extensions/pull/2078)
> (merged 2026-06-19). This plan bumps that descriptor **v0.14.1 → v0.15.0**.

## Why v0.15.0 (not v0.14.3)

See `docs/RELEASE_NOTES_v0.15.0.md` for the full reasoning. Short version:
`main` has moved past `v0.14.2` with a user-visible surface change — DuckDB
v1.5.2/v1.5.3 support was dropped (official matrix now v1.5.4/v1.5.5 only) —
plus CI/doc/tooling hygiene (`extension-ci-tools` pin alignment,
`SECURITY.md` fix, `.clang-format` + full `src/` reformat, informational
sanitizer CI, `CHANGELOG.md`, `ARCHITECTURE.md` Appendix A/B corrections).
The dropped-version-support change alone is enough to make this a minor
bump under the project's loose (non-strict) semver convention — a narrower
supported-version claim is user-visible, not an internal chore.

## Current community state

- **Live community baseline:** `salesforce` @ `v0.14.1` (merged via #2078).
  Installable today: `INSTALL salesforce FROM community; LOAD salesforce;`.
- **Proposed update ref:** `v0.15.0` (own-repo tag not yet cut — see
  Preconditions below; this plan is prepared ahead of the tag so review can
  happen before anything is cut).
- **Real descriptor unchanged:** `docs/community/description.yml` still
  declares `version: 0.14.1` / `repo.ref: v0.14.1`. The v0.15.0 values live
  only in the review draft `docs/community/description.v0.15.0.draft.yml`
  until C.5 GO.

## Preconditions for the update (status)

- [ ] **`v0.15.0` tagged on `flozer`** (own-repo tag not yet cut — pending
      human decision on when to cut, per PM->DEV #006 restriction: this
      cycle prepares, does not tag).
- [x] Internal version consistency ready for the `v0.15.0` ref: `vcpkg.json`
      `version-string: 0.15.0` (this cycle).
- [ ] Release assets published for `v0.15.0` (`release-assets.yml`, gated on
      the tag — not yet cut).
- [x] Offline mock suite green post-reformat (cycle #006): see
      `docs/RELEASE_NOTES_v0.15.0.md` for the exact assertion count and CI
      run URL — same count as pre-reformat, confirming clang-format changed
      no behavior.
- [ ] **Fresh matrix CI green at the `v0.15.0` ref** — Main Distribution
      Pipeline (`linux_amd64`, `windows_amd64`, `osx_arm64` × DuckDB
      v1.5.4/v1.5.5), mock-only, no `SF_LIVE_*`. *(The cycle #006 branch run
      cited in the release notes is auxiliary build-proof of the identical
      code pre-tag; the SUBMISSION-ref proof must be re-run at the actual
      `v0.15.0` tag once cut.)*
- [ ] **Public shallow-clone of `v0.15.0`** re-verified (anonymous;
      `vcpkg.json` `version-string: 0.15.0`) — only possible once the tag
      exists and is public.
- [x] **Descriptor draft reviewed** (`description.v0.15.0.draft.yml`,
      created this cycle — pending PM/human review).
- [ ] **Maintainer C.5 GO** for the update publication.

## What gets changed (ONLY on GO)

Exactly one file in a fork of `duckdb/community-extensions`, **edited** (the
path already exists from #2037/#2078):

- `extensions/salesforce/description.yml` ← contents of
  `docs/community/description.v0.15.0.draft.yml` (after it is promoted to
  the real `docs/community/description.yml`).

No source, no tests, no other docs are copied. Community CI checks out
`flozer/duckdb-salesforce` at `repo.ref: v0.15.0` and rebuilds + re-signs.

## Steps to execute (ONLY on GO — listed, not run)

1. Maintainer cuts the `v0.15.0` tag on `flozer/duckdb-salesforce` (own-repo
   release, `release-assets.yml`), separate from and prior to this plan's
   execution.
2. Re-run Main Distribution Pipeline at the `v0.15.0` tag, confirm green on
   all three platforms × v1.5.4/v1.5.5.
3. Promote the reviewed draft to the real `docs/community/description.yml`
   (`version: 0.15.0`, `repo.ref: v0.15.0`) — a separate, GO-gated commit on
   `flozer`.
4. Fork / update fork of `duckdb/community-extensions`.
5. Branch, e.g. `update-salesforce-0.15.0`.
6. Edit `extensions/salesforce/description.yml` = verbatim copy of the
   promoted `docs/community/description.yml`.
7. Commit (`Update salesforce extension to 0.15.0`), push the branch to the
   fork.
8. Open a PR into `duckdb/community-extensions:main` with the body below.
9. Respond to community-CI / reviewer feedback. Do not merge (maintainers
   do).

## PR body (draft)

> **Extension:** `salesforce` (update) — read-only Salesforce access as
> DuckDB SQL tables over REST + Bulk. Already in community at `v0.14.1`
> (#2078); this bumps it to `v0.15.0`.
>
> **Repo / ref:** `flozer/duckdb-salesforce` @ `v0.15.0` (annotated tag).
> **License:** MIT. **Dependency:** OpenSSL (via `vcpkg.json`).
> **Platforms:** `linux_amd64`, `windows_amd64` (baseline) + `osx_arm64`
> (extra). Excluded: `osx_amd64`, arm-linux, musl, wasm, mingw,
> windows_arm64.
>
> **New since v0.14.1:** `extension-ci-tools` pin alignment and
> `SECURITY.md` accuracy fix; official supported-version matrix narrowed to
> DuckDB **v1.5.4/v1.5.5** (v1.5.2/v1.5.3 dropped — pre-existing Windows/MSVC
> `fmt` build failure on those versions, resolved by scope reduction, not a
> code fix); `.clang-format` added and all of `src/` reformatted to it (no
> behavior change); informational, non-blocking ASan/UBSan/TSan CI added;
> `CHANGELOG.md` added; `docs/ARCHITECTURE.md` roadmap and Appendix A/B
> factual corrections. No SOQL/scan/transport behavior change vs `v0.14.1`.
>
> **Evidence:** offline mock suite green (assertion count / CI run — fill at
> submission from `docs/RELEASE_NOTES_v0.15.0.md`); matrix CI green at
> `v0.15.0` (run TBD — fill at submission, must be re-run at the actual tag).
> Source repo/tag public-clone validated.
>
> **Known caveats (declared, unchanged):**
> - macOS live TLS not validated in CI (OpenSSL-via-vcpkg does not read the
>   Keychain); `SSL_CERT_FILE` workaround documented + actionable error
>   hint.
> - JWT bearer offline-covered (real RS256 over a test key + mock token),
>   not live-validated against a pre-authorized Connected App.
> - Blob/base64 body fields not byte-readable (documented, guarded, by
>   design).
> - A pre-existing Linux Debug-build-only linker curiosity
>   (`multiple definition of LogicalType::VARCHAR`, GCC `STB_GNU_UNIQUE` +
>   unoptimized build + static-linked extension) affects only informational,
>   non-gating Debug/sanitizer CI jobs in this repo — never the Release build
>   this descriptor ships, and not something community CI (always Release)
>   would ever hit either.
> - Node.js action-deprecation warnings from the CI runner (non-blocking).

(Internal note, not for the PR body: full R-010 root-cause writeup lives in
this repo's private, untracked `AGENTES.MD` — see cycle #005.)

## Caveats to declare (summary)

macOS live TLS (workaround documented) · JWT not live-validated · blob
bodies not byte-readable (by design) · Debug-only linker curiosity
(Release unaffected) · Node action warnings · excluded platforms.

## Guardrails

No PR is opened, no fork branch is pushed, and the real
`docs/community/description.yml` is not changed until the maintainer says
GO and confirms the exact PR body above (or an edited version of it). This
document, the draft descriptor, and the release notes are the only
artifacts this cycle produces toward a future submission.

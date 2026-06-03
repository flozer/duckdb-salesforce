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
- [ ] CI green on the full matrix: **Windows + Linux × DuckDB v1.5.2, v1.5.3**.
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

- [ ] `description.yml` (or equivalent) — name, description, version, maintainers,
      repo, license, language.
- [ ] LICENSE present and compatible.
- [ ] `SECURITY.md` present (vuln reporting, TLS-on, no-secret-logging).
- [ ] No secrets, tokens, org identifiers, or customer data anywhere in the repo
      or CI logs.
- [ ] README documents auth (refresh-token OAuth), read-only scope, limitations.

## Behaviour / safety

- [ ] Read-only: all mutating catalog ops throw.
- [ ] Errors are secret-free (no bearer/body/secret in messages).
- [ ] Quota governor + REQUEST_LIMIT_EXCEEDED handling documented.

## Sign-off

- [ ] Maintainer reviews this checklist and the latest release notes.
- [ ] **Explicit human GO** recorded to submit to `duckdb/community-extensions`.

Until the final box is checked by a human, the extension stays **local /
`flozer`-only**.

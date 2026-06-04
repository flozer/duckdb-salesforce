# Release notes — v0.9.0 (DRAFT — NOT TAGGED)

> **Status: DRAFT.** This release is **not tagged** and **not published**. No
> remote CI run, no GitHub release, nothing submitted to
> `duckdb/community-extensions`. This document summarizes the work accumulated
> since `v0.8.1` so a tag/smoke decision can be made deliberately. Until a
> maintainer GO (see *Community status*), it stays a draft.

Range: `v0.8.1..HEAD` (commits `bf99798` → `a4b23a0`).

---

## Highlights

A read-only DuckDB ↔ Salesforce bridge gains analytical aggregates (without any
optimizer), richer authentication, relationship traversal + diagnostics, and a
macOS TLS path — all validated by the offline mock suite.

## New features

### Querying

- **`sf_query_mode = 'query' | 'queryAll'`** (`bf99798`) — `queryAll` returns
  archived + soft-deleted (`IsDeleted = true`) records. Applies to REST, Bulk,
  and the COUNT()/MIN-MAX probes so counts/ranges stay consistent.
- **Explicit server-side aggregates — `salesforce_aggregate()`** (`deda8ab`,
  GROUP BY in `a4b23a0`):
  ```
  salesforce_aggregate(catalog, object, aggregates [, filter [, group_by]])
  ```
  Runs `SELECT <group_by>, <aggregates> FROM <object> [WHERE <filter>]
  [GROUP BY <group_by>]` over an already-ATTACHed catalog's authenticated
  session (no re-auth, no secrets in the call). Returns one row per group, one
  VARCHAR column per term (group columns first, then aggregates named by alias
  or `expr0/1/...`). Aggregate functions: MIN, MAX, SUM, AVG, COUNT,
  COUNT_DISTINCT. Honors `sf_query_mode`; records the SOQL in diagnostics.
  Opt-in by design — **not** transparent pushdown.

### Relationships

- **Grandparent traversal — `sf_relationship_depth` (1..2)** (`464b05b`) —
  opt-in (`sf_relationships = 'parent'`); depth 2 expands a nested parent STRUCT
  (e.g. `Contact.Account.Owner.Name`) over REST nested JSON and Bulk nested CSV.
  Single-target only; polymorphic / self / cycle skipped.
- **Relationship diagnostics — `salesforce_relationships()`** (`6d12aa1`) — one
  `config` row (mode, effective depth, expanded/skipped counts — always emitted,
  even when off) plus one `relationship` row per reference field considered
  (expanded with `field_count`, or skipped with a reason: `polymorphic`,
  `self_reference`, `cycle`, `name_collision`, `parent_not_describable`,
  `no_fields`, `no_relationship_name`). Expanded rows carry an over-fetch note.

### Authentication (Auth UX)

- **`auth_source = 'options' | 'env' | 'sfdx_url'`** (`b8a7b6f`) — credential
  source selection without changing the refresh-token flow. `env` reads
  `SF_CLIENT_ID` / `SF_CLIENT_SECRET` / `SF_REFRESH_TOKEN` (+ optional
  `SF_LOGIN_URL`); `sfdx_url` reads `SF_SFDX_AUTH_URL`. Clearer, secret-free
  OAuth errors (`invalid_grant`, `invalid_client`).
- **`auth_source = 'jwt'` — OAuth 2.0 JWT bearer** (`6cd7923`) — RS256-signed
  assertion, no refresh token; ideal for headless/CI. Key path from
  `SF_JWT_KEY_FILE` (recommended) or inline `private_key_file` (local dev);
  unencrypted PKCS#1/PKCS#8 only. The key, JWT, and assertion are never logged
  or echoed. The Connected App must be pre-authorized.

### Platform / TLS

- **macOS live-TLS hint** (`26331ff`) — on a macOS certificate-verification
  failure, the error now suggests `SSL_CERT_FILE` (Homebrew/certifi bundle).
  Verification stays ON — this selects trust anchors, it is not a bypass. The
  zero-config Keychain trust store (Security.framework) remains a follow-up.

### Hardening / housekeeping

- Bulk CSV edge-case hardening + REST/Bulk type parity (test-only) (`e385165`);
  lenient-quote parse recorded as known behavior (`5fcde50`).
- Distribution pipeline made manual-only (`workflow_dispatch`) (`f62f34f`) —
  acceptance is now offline-local-green; remote CI is opt-in.
- PEM test fixtures pinned binary via `.gitattributes` (`7f1c97e`).

## Deferred (require a DuckDB OptimizerExtension)

Recorded in `docs/ROADMAP.md` (`16da859`, `5da832e`):

- **Transparent `COUNT(field)` pushdown** — `COUNT(*)` already covers the main
  case via the zero-column trick; `COUNT(field)` cannot reuse it.
- **Transparent `MIN`/`MAX`/`SUM`/`AVG` and `GROUP BY` pushdown** — DuckDB runs
  the aggregate above the scan, which sees a normal projected column; there is
  no aggregate-pushdown hook in the `TableFunction` API. Enabling these would
  require intercepting and rewriting `Aggregate → Scan` (an OptimizerExtension).

Per ACTION_GUIDE (strengthen the simple core before adding heavy layers), these
stay deferred. The **explicit** `salesforce_aggregate()` covers the practical
need (server-side aggregates, no rows dragged down) without that machinery.
`COUNT(field)` / `MIN`/`MAX`/etc remain correct today via the normal scan +
DuckDB aggregation.

## Test evidence (offline)

- **Offline mock suite: 28 test files, 775 assertions — green** (local Windows,
  Release build, DuckDB v1.5.3 pin). The suite never contacts Salesforce and
  needs no secrets; the JWT tests RS256-sign a throwaway test-only key.
- Run locally:
  ```sh
  # build (see docs/INSTALL.md), then per file:
  build/release/test/unittest.exe "test/sql/salesforce_<name>.test"
  ```
- CI validates build + offline suite on `linux_amd64`, `windows_amd64`
  (baseline) and `osx_arm64` (extra) across DuckDB v1.5.2 / v1.5.3 — but the
  pipeline is **manual-only** now, so no run is attached to this draft.

## Live-smoke checklist (optional, pre-tag)

Not required for the draft; run against a real org before tagging if desired.
No secrets in the repo or CI.

- [ ] `auth_source 'options'` ATTACH + a trivial `SELECT` from a real org.
- [ ] `auth_source 'env'` and `'sfdx_url'` ATTACH.
- [ ] `auth_source 'jwt'` ATTACH against a pre-authorized Connected App.
- [ ] `sf_query_mode = 'queryAll'` returns archived/deleted rows.
- [ ] `sf_relationships = 'parent'`, depth 1 and 2, nested field access.
- [ ] `salesforce_aggregate()` with/without filter and GROUP BY.
- [ ] macOS live TLS with `SSL_CERT_FILE` set.
- [ ] Verify no token/secret/JWT/key appears in any error or log.

## Community status

**Blocked by C.5 (explicit human GO).** A submission to
`duckdb/community-extensions` is human-gated and has not been prepared as a
branch or PR anywhere. See `docs/PRE_COMMUNITY_CHECKLIST.md`. The only
remaining substantive platform note is the macOS zero-config trust store
(option B, follow-up) — not a parity blocker.

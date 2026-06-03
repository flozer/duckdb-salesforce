# Contributing

Thanks for helping improve `duckdb-salesforce`.

This project is a DuckDB extension that talks to Salesforce over the official
REST and Bulk APIs, so correctness, reproducible (offline) tests, and
conservative pushdown rules matter more than clever shortcuts.

## Before Opening a Pull Request

1. Open an issue or draft PR for behavior changes, new pushdown rules, type
   mapping changes, transport/quota changes, or public API changes.
2. Keep pull requests focused. Avoid mixing refactors with feature changes.
3. **Never** include real Salesforce orgs, customer data, production URLs,
   access/refresh tokens, client secrets, or any credential.
4. Use the mock hooks (`sf_mock_*`) and synthetic fixtures under `test/sql/`.
5. Update the public function manual when changing user-facing SQL. Project
   premise: every new function, setting, parameter, output column, or semantic
   change must update `docs/en/function_manual.md` **and** its
   `docs/pt/function_manual.md` counterpart (see `docs/DOCS_PARITY.md`).

## Development Setup

Clone with submodules:

```bash
git clone https://github.com/flozer/duckdb-salesforce.git
cd duckdb-salesforce
git submodule update --init --recursive
```

Build with the DuckDB extension harness:

```bash
make release        # or: make debug
```

TLS is provided by **OpenSSL** (pulled via the `vcpkg.json` manifest); the HTTP
client is vendored header-only `httplib` under `third_party/httplib/`. There is
**no** Salesforce client library to install — the only runtime needs are network
access and OAuth refresh-token credentials. See the platform guides:

- `docs/en/guide_linux.md`
- `docs/en/guide_windows.md`

## Testing

The offline test suite is **mock-only**: it drives the `sf_mock_*` hooks and
**never contacts Salesforce**, needs **no secrets**, and is what CI runs.

```bash
make test_release
```

Cover the area you change, e.g.:

- `test/sql/salesforce_scan.test`, `salesforce_attach.test`
- `salesforce_pushdown.test`, `salesforce_pushdown_more.test`
- `salesforce_bulk.test`, `salesforce_bulk_lazy.test`, `salesforce_bulk_chunks.test`
- `salesforce_quota.test`, `salesforce_cost.test`, `salesforce_count.test`
- `salesforce_tooling.test`, `salesforce_relationships.test`,
  `salesforce_transport_auto.test`

**Live validation is manual-only**, run only against an org the maintainer is
authorized to use (refresh-token OAuth). `*_live.test` files skip without a live
org. Automated CI must never contact Salesforce or require secrets.

## Pushdown Rules

Only push a predicate (or aggregate) to SOQL when the result is semantically
equivalent to DuckDB evaluation. If in doubt, leave it **residual** so DuckDB
re-checks it. Take special care with:

- non-filterable fields (predicate must stay residual);
- `LIKE` (Salesforce is case-insensitive → push as a superset prefilter, keep
  residual), `IN`/`OR` supersets;
- `COUNT(*)` pushdown (only when there is no residual filter);
- Bulk PK chunking (`Id` range boundaries must be valid-length Salesforce Ids);
- the per-job quota governor (Bulk starts only).

## Security

Follow `SECURITY.md` for vulnerability reporting. Credentials live in memory
only and must never be logged; TLS verification is always on. If a change
affects SOQL generation, credential handling, the HTTP transport, or GitHub
Actions, call that out explicitly in the PR description.

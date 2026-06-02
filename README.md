# duckdb-salesforce

A DuckDB extension that exposes Salesforce orgs as queryable SQL tables, via the
official Salesforce REST / Bulk APIs. Architectural sibling of
[duckdb-firebird](https://github.com/flozer/duckdb-firebird) — same
catalog/storage + table-function scanner + pushdown design, different backend.

> **Status: v0.1 (read-only REST) — DEV / SANDBOX ONLY. Not production-ready.**
> ATTACH authenticates via OAuth, sObjects are resolved on demand, and
> `SELECT * FROM salesforce.<Object>` returns typed rows over the REST query
> API, with SOQL projection + a conservative predicate pushdown. Use only
> against **sandbox / scratch** orgs with throwaway Connected App credentials.
> See [`v0.1-readonly-rest`](https://github.com/flozer/duckdb-salesforce/milestone/1)
> and the [limitations](#v01-limitations) below.

## Usage

```sql
LOAD salesforce;

-- Use a sandbox Connected App; for a sandbox add login_url 'https://test.salesforce.com'.
ATTACH 'salesforce://production' AS sf (TYPE salesforce,
    client_id 'xxx', client_secret 'xxx', refresh_token 'xxx');

SELECT Id, Name FROM sf.Account WHERE Name = 'Acme';
```

## Pushdown (v0.1)

Pushdown to SOQL is a best-effort over-fetch optimisation; anything not pushed is
applied by DuckDB residually, so results are always correct.

| Operation | v0.1 |
| --- | --- |
| Projection (`SELECT a, b`) | ✅ pushed to SOQL |
| Predicate `=, <>, <, <=, >, >=` (filterable field, constant) | ✅ pushed |
| `IS NULL` / `IS NOT NULL` | ✅ pushed (`= null` / `!= null`) |
| `AND` of pushable predicates | ✅ pushed |
| `OR`, `IN`, `LIKE`, functions, casts, nested expr | ⛔ residual (DuckDB) |
| Predicate on a non-filterable field | ⛔ residual |
| `WHERE` longer than 4000 chars | ⛔ residualised (guard) |
| `LIMIT n` | ⛔ residual — this DuckDB build does not expose the query LIMIT to a table function (applied by DuckDB after the scan) |

## Architecture

The full design (22 deliverables: auth flow, query flow, REST/Bulk/Tooling/
Metadata strategies, pushdown table, cache, C++ structure, roadmap, quotas,
risks, tests) lives in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The v0.1
delivery track (Definition of Done, non-goals, security gate) is **Appendix C**.

## Build

```sh
git submodule update --init --recursive
make release        # or: make debug
```

The live HTTPS transport uses vendored [httplib](third_party/httplib) (MIT) +
**OpenSSL**, pulled via vcpkg manifest mode (`vcpkg.json`). On Windows without
GNU `make`, configure DuckDB directly inside a VS dev shell with the vcpkg
toolchain (`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`,
`-DVCPKG_TARGET_TRIPLET=x64-windows-static`) — the triplet must match DuckDB's
static (`/MT`) MSVC runtime; OpenSSL is built on first configure. TLS
certificate verification is always on; there is no insecure build flag.

Pinned via the `duckdb` and `extension-ci-tools` submodules to the commits
the v0.1 scaffold was built and tested against:

- `duckdb` @ `0a8a19486d` (`v1.5.2-6640-g0a8a19486d`)
- `extension-ci-tools` @ `a4373c6e`

The `StorageExtension::Register` registration path and the
`attach` + `create_transaction_manager` dispatch requirement are verified
against this DuckDB build (see `src/salesforce_storage.cpp`).

## Testing

The whole suite runs **fully offline** — every test mocks the Salesforce HTTP
layer, so CI never contacts a real org.

```sh
make test_debug              # CI path (GNU make + extension-ci-tools)
# or run the unittest binary directly against the SQL tests:
build/release/test/unittest "test/sql/salesforce_scan.test"
```

The real network paths are exercised only by the gated `*_live.test` files,
which are **skipped** unless `SF_LIVE_CLIENT_ID` / `SF_LIVE_CLIENT_SECRET` /
`SF_LIVE_REFRESH_TOKEN` are set — **never set these in CI**. As of v0.1 the
offline suite is 166 assertions across 8 files, with 4 live files skipped.

For a repeatable, secret-safe manual sandbox validation (and the criteria to
tag `v0.1.0`), see [SMOKE.md](SMOKE.md).

## v0.1 limitations

Known and intentional for this cut (see `docs/ARCHITECTURE.md` Appendix C):

- **Dev / sandbox only.** Not production-ready; use throwaway sandbox creds.
- **`LIMIT` is residual**, not SOQL pushdown — this DuckDB build does not expose
  the query `LIMIT` to a table function, so DuckDB applies it after the scan
  (correct, but the full page is still fetched).
- **`SHOW TABLES` is lazy / partial.** Objects resolve on first reference; there
  is no upfront global object listing, so the catalog only lists sObjects
  already referenced this session.
- **REST only.** No Bulk API 2.0, GraphQL, Tooling or Metadata API yet; no
  relationship/join traversal; no persisted metadata cache.
- **Read-only.** All catalog mutations (CREATE/INSERT/UPDATE/DELETE/...) throw.

## Community publication

Per `docs/ARCHITECTURE.md` Appendix C.5, there is **no** push, PR, tag, or
release to `duckdb/community-extensions` without explicit human go/no-go after
multi-test evidence. Development stays in this repository.

## License

See [LICENSE](LICENSE) (to be added).

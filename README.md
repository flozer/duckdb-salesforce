# duckdb-salesforce

A DuckDB extension that exposes Salesforce orgs as queryable SQL tables, via the
official Salesforce REST / Bulk APIs. Architectural sibling of
[duckdb-firebird](https://github.com/flozer/duckdb-firebird) — same
catalog/storage + table-function scanner + pushdown design, different backend.

> **Status: v0.1 (read-only REST) — early, not production-hardened.**
> ATTACH authenticates via OAuth, sObjects are resolved on demand, and
> `SELECT * FROM salesforce.<Object>` returns typed rows over the REST query
> API, with SOQL projection + a conservative predicate pushdown.
> **Live validation is manual-only** and must be run only against an org the
> maintainer is authorized to use; **automated CI never contacts Salesforce or
> requires secrets**. See
> [`v0.1-readonly-rest`](https://github.com/flozer/duckdb-salesforce/milestone/1)
> and the [limitations](#v01-limitations) below.

## Usage

```sql
LOAD salesforce;

-- Use a sandbox Connected App; for a sandbox add login_url 'https://test.salesforce.com'.
ATTACH 'salesforce://production' AS sf (TYPE salesforce,
    client_id 'xxx', client_secret 'xxx', refresh_token 'xxx');

SELECT Id, Name FROM sf.Account WHERE Name = 'Acme';
```

## Transport: REST vs Bulk (v0.3)

A scan runs over one of two transports, chosen by the `sf_force_transport`
setting. Both use the **same** optimised SOQL — projection + predicate pushdown
apply identically — so only the delivery mechanism differs.

```sql
SET sf_force_transport = 'rest';   -- default: lazy REST /query + queryMore
SET sf_force_transport = 'bulk';   -- Bulk API 2.0 query job (CSV results)
```

| | `rest` (default) | `bulk` |
| --- | --- | --- |
| Mechanism | `/query` + `queryMore` pages | create job → poll → download CSV (`Sforce-Locator` paging) |
| Streaming | lazy — stops early on small `LIMIT` | eager — whole result downloaded before the first row is emitted |
| Best for | interactive queries, small/medium results | large extractions, `CREATE TABLE AS`, `COPY` |
| 401 handling | refresh-token retry (once) | same — on create, poll, and each results page |

Notes / limitations of the `bulk` path:

- **`LIMIT` is not honoured server-side.** Bulk API 2.0 query jobs ignore SOQL
  `LIMIT`; the full result set is fetched and `LIMIT` is applied residually by
  DuckDB. Use `rest` when you want a small `LIMIT` to read little.
- The result is fetched **eagerly** in `InitGlobal` (all pages, following the
  `Sforce-Locator`), so memory scales with the result size. v0.3 does not yet
  stream Bulk pages into the scan.
- A `Failed`/`Aborted` job raises a clean, secret-free error.
- Auto-selecting the transport by query shape is deferred (v0.3 §2); for now it
  is explicit via the setting.

## Pushdown (v0.1)

Pushdown to SOQL is a best-effort over-fetch optimisation; anything not pushed is
applied by DuckDB residually, so results are always correct.

| Operation | v0.1 |
| --- | --- |
| Projection (`SELECT a, b`) | ✅ pushed to SOQL |
| Predicate `=, <>, <, <=, >, >=` (filterable field, constant) | ✅ pushed |
| `IS NULL` / `IS NOT NULL` | ✅ pushed (`= null` / `!= null`) |
| `AND` of pushable predicates | ✅ pushed |
| `IN (...)` (constant list, up to 200 items) | ✅ pushed as a superset **prefilter**, kept residual (DuckDB refines) |
| `LIKE 'A%'` / `'%z'` / `'%m%'` | ✅ pushed as a superset **prefilter**, kept residual — Salesforce `LIKE` is case-insensitive, so DuckDB re-applies it locally to refine |
| `OR` of pushable predicates (all children safe) | ✅ pushed as superset, kept residual; mixed safe/unsafe → whole `OR` residual |
| functions / casts / `NOT` / regex / nested-unsupported | ⛔ residual (DuckDB) |
| Predicate on a non-filterable field | ⛔ residual |
| `WHERE` longer than 4000 chars | ⛔ residualised (guard) |
| `LIMIT n` | ⚠️ not pushed to SOQL (this DuckDB build exposes no LIMIT hook), **but the scan is lazy** — it streams pages and may stop before fetching later pages, so a small LIMIT no longer reads the whole object |

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

Pinned via the `duckdb` and `extension-ci-tools` submodules to the
**DuckDB v1.5.3 release** (the minimum supported version):

- `duckdb` @ `14eca11bd9` (release **v1.5.3**)
- `extension-ci-tools` @ `18c54662`

DuckDB extensions are **version-locked**: a `.duckdb_extension` built against a
given DuckDB release only loads in that release. "Supporting 1.5.3 onward" means
**rebuilding + testing per release**, not shipping one universal binary.

## Supported DuckDB versions

| DuckDB release | Status |
| --- | --- |
| **v1.5.3** | ✅ baseline (committed submodule pin), build + offline suite green |
| v1.5.2 | ✅ build + offline suite green (matrix) |

The repo's `duckdb` submodule pins **v1.5.3** (`14eca11bd9`); `extension-ci-tools`
pins `18c54662`. To build/test against a matrix of releases locally (each in its
own `build/matrix/<tag>` dir, the committed pin restored at the end):

```powershell
pwsh -File scripts/build_matrix.ps1            # default: v1.5.2, v1.5.3
```

Newer releases are added to the matrix as they ship; if a future release needs
an API adaptation, that is tracked as its own issue (the baseline must stay
green). The `StorageExtension::Register` path and the `attach` +
`create_transaction_manager` dispatch requirement are verified against this
build (see `src/salesforce_storage.cpp`).

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

- **Live validation is manual-only.** Run only against an org you are
  authorized to use; automated CI never contacts Salesforce or uses secrets.
  Not production-hardened.
- **`LIMIT` is not pushed to SOQL** (this DuckDB build exposes no LIMIT hook),
  but the **scan is lazy**: it streams query pages and may stop before fetching
  later pages, so a small `LIMIT` no longer reads the whole object. A full
  `COUNT(*)`/unfiltered scan still walks every page.
- **Object listing** is available via `duckdb_tables()` and
  `information_schema.tables` — the first listing runs one global describe
  (`GET /sobjects`, queryable objects only), cached until DETACH. **Columns are
  lazy**: `information_schema.columns` is empty for an object until it is
  referenced (then a per-object describe fills its schema). `SHOW ALL TABLES`
  does not show name-only (unreferenced) entries — use `duckdb_tables()`.
- **REST only.** No Bulk API 2.0, GraphQL, Tooling or Metadata API yet; no
  relationship/join traversal.
- **Metadata cache is in-memory only.** Each sObject's schema (describe) is
  cached per ATTACH and reused (described once per object); nothing is persisted
  to disk, and record/data are never cached. The cache is dropped on DETACH.
- **Read-only.** All catalog mutations (CREATE/INSERT/UPDATE/DELETE/...) throw.

## Community publication

Per `docs/ARCHITECTURE.md` Appendix C.5, there is **no** push, PR, tag, or
release to `duckdb/community-extensions` without explicit human go/no-go after
multi-test evidence. Development stays in this repository.

## License

See [LICENSE](LICENSE) (to be added).

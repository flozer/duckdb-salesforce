# duckdb-salesforce

A DuckDB extension that exposes Salesforce orgs as queryable SQL tables, via the
official Salesforce REST / Bulk APIs. Architectural sibling of
[duckdb-firebird](https://github.com/flozer/duckdb-firebird) — same
catalog/storage + table-function scanner + pushdown design, different backend.

> **Status: v0.1 scaffold — DEV / SANDBOX ONLY. Not production-ready.**
> This cut only proves the extension builds, loads, and registers the
> `salesforce` storage type. Authentication, schema discovery and table
> scanning are **not implemented yet**; `ATTACH` fails with a clear
> not-implemented message. See the
> [`v0.1-readonly-rest`](https://github.com/flozer/duckdb-salesforce/milestone/1)
> milestone for scope and progress.

## Target experience (not yet functional)

```sql
INSTALL salesforce;
LOAD salesforce;

ATTACH 'salesforce://production'
    (client_id 'xxx', client_secret 'xxx', refresh_token 'xxx');

SELECT Id, Name FROM salesforce.Opportunity
WHERE LastModifiedDate >= '2025-01-01';
```

## Architecture

The full design (22 deliverables: auth flow, query flow, REST/Bulk/Tooling/
Metadata strategies, pushdown table, cache, C++ structure, roadmap, quotas,
risks, tests) lives in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The v0.1
delivery track (Definition of Done, non-goals, security gate) is **Appendix C**.

## Build

```sh
git submodule update --init --recursive
make release        # or: make debug
make test_debug     # runs test/sql/*.test against the debug build
```

Pinned via the `duckdb` and `extension-ci-tools` submodules to the commits
the v0.1 scaffold was built and tested against:

- `duckdb` @ `0a8a19486d` (`v1.5.2-6640-g0a8a19486d`)
- `extension-ci-tools` @ `a4373c6e`

The `StorageExtension::Register` registration path and the
`attach` + `create_transaction_manager` dispatch requirement are verified
against this DuckDB build (see `src/salesforce_storage.cpp`).

## License

See [LICENSE](LICENSE) (to be added).

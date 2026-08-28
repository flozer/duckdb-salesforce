<div align="center">
  <h1>duckdb-salesforce</h1>
  <p><strong>Query Salesforce directly from <a href="https://github.com/duckdb/duckdb">DuckDB</a>.</strong></p>
  <p>
    Federated analytics over Salesforce, Parquet, CSV, S3, and local DuckDB
    tables, with REST/Bulk transport, pushdown, metadata helpers, OAuth/JWT
    auth, and native <code>ATTACH</code>.
  </p>
  <p>
    <a href="LICENSE"><img alt="license MIT" src="https://img.shields.io/badge/license-MIT-green.svg"></a>
    <a href="https://github.com/flozer/duckdb-salesforce/releases/tag/v0.14.2"><img alt="release v0.14.2" src="https://img.shields.io/badge/release-v0.14.2-blue.svg"></a>
    <a href="https://github.com/flozer/duckdb-salesforce/actions/workflows/MainDistributionPipeline.yml"><img alt="Build + Test Linux Windows macOS" src="https://github.com/flozer/duckdb-salesforce/actions/workflows/MainDistributionPipeline.yml/badge.svg"></a>
    <a href="https://github.com/duckdb/community-extensions/pull/2078"><img alt="DuckDB community merged" src="https://img.shields.io/badge/DuckDB%20community-merged-brightgreen.svg"></a>
    <a href="https://duckdb.org/community_extensions/download_metrics"><img alt="DuckDB Community total downloads" src="https://img.shields.io/endpoint?url=https%3A%2F%2Fraw.githubusercontent.com%2Fflozer%2Fduckdb-salesforce%2Fmain%2F.github%2Fbadges%2Fdownloads.json"></a>
  </p>
  <p>
    <a href="docs/en/usage_guide.md">Usage guide</a> |
    <a href="docs/en/function_manual.md">Function manual</a> |
    <a href="docs/ROADMAP.md">Roadmap</a> |
    <a href="docs/pt/usage_guide.md">Guia PT</a> |
    <a href="CONTRIBUTING.md">Contributing</a> |
    <a href="CODE_OF_CONDUCT.md">Code of conduct</a> |
    <a href="SECURITY.md">Security</a>
  </p>
</div>

`duckdb-salesforce` is a DuckDB extension for read-only analytics on Salesforce
data. It lets analysts keep Salesforce as the operational source of truth while
using DuckDB as the local OLAP engine for ad hoc analysis, BI staging, Parquet
exports, lakehouse-style snapshots, and cross-source joins.

`duckdb-salesforce` is the first Salesforce extension published in the DuckDB
Community Extensions registry, added via
[duckdb/community-extensions#2037](https://github.com/duckdb/community-extensions/pull/2037)
and updated to `v0.14.1` via
[duckdb/community-extensions#2078](https://github.com/duckdb/community-extensions/pull/2078).

The extension is a bridge, not an ETL platform: Salesforce access, transport,
authentication, metadata, and safe pushdown live here; joins, aggregations,
materialization, files, and downstream analytics stay in DuckDB.

## Principles

- **Read-only by design** - optimized for analytics without mutating Salesforce
  records or metadata.
- **Push work to Salesforce when safe** - projection, predicates, `COUNT(*)`,
  `queryAll`, explicit aggregates, Bulk streaming, and PK chunking reduce wire
  traffic.
- **Keep DuckDB in charge of analytics** - joins, BI extracts, Parquet/S3,
  materialized tables, and local transformations use DuckDB's vectorized engine.
- **Respect Salesforce realities** - API quota, Bulk job lifecycle, soft-deleted
  records, relationship describes, picklists, record types, blobs, and auth
  modes are surfaced explicitly.
- **Secret-safe by default** - credentials come from options, environment, SFDX
  auth URL, or JWT key files; tokens, keys, JWTs, and org data are never logged.
- **Published in DuckDB Community Extensions** - added through
  [`duckdb/community-extensions#2037`](https://github.com/duckdb/community-extensions/pull/2037)
  and updated through
  [`duckdb/community-extensions#2078`](https://github.com/duckdb/community-extensions/pull/2078);
  the pinned `v0.14.1` source ref is public-clone validated and CI green.

## Features

| Area | What works |
|---|---|
| Native DuckDB access | `ATTACH ... (TYPE salesforce)` exposes Salesforce objects as DuckDB tables. |
| Auth | OAuth refresh-token, SFDX auth URL, environment-based credentials, and JWT bearer. |
| REST scans | Lazy pagination, projection pushdown, predicate pushdown, `LIMIT` fetch reduction, `queryAll`. |
| Bulk scans | Bulk API 2.0 query, lazy result streaming, sequential/parallel PK chunking, quota guard. |
| Transport selection | `sf_force_transport = 'rest' \| 'bulk' \| 'auto'` with diagnostics explaining choices. |
| Relationships | Parent and grandparent traversal as nested `STRUCT`, with skip/expand diagnostics. |
| Metadata Engine v2 | Shared per-catalog read-only metadata cache; `salesforce_metadata_objects()` / `salesforce_metadata_fields()` for analysts; `salesforce_refresh_metadata()` invalidation. |
| Aggregates | Transparent `COUNT(*)` pushdown and explicit `salesforce_aggregate()` with optional `GROUP BY`. |
| Diagnostics | Last SOQL, transport, quota, query cost, page counts, relationship decisions. |
| Query explainability | `salesforce_query_explain()` — read-only, last-scan, field-by-field view of pushed vs. residual filters, projection, relationship, count, and transport (diagnostic-only; no scan behavior change). |
| Testing | Offline mock suite plus validated live smoke; CI covers Linux, Windows, and macOS arm64. |

## Quick Start

Install the signed community artifact:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

For local builds, see [docs/INSTALL.md](docs/INSTALL.md).

```sql
SET allow_unsigned_extensions = true;
LOAD '/path/to/salesforce.duckdb_extension';
```

Attach an org with inline options:

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce,
  client_id '...',
  client_secret '...',
  refresh_token '...',
  login_url 'https://login.salesforce.com');
```

Or keep secrets out of SQL with environment variables:

```powershell
$env:SF_CLIENT_ID='...'
$env:SF_CLIENT_SECRET='...'
$env:SF_REFRESH_TOKEN='...'
$env:SF_LOGIN_URL='https://login.salesforce.com'
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

Then query Salesforce like regular DuckDB tables:

```sql
SELECT Id, Name, Industry
FROM sf.Account
WHERE Name LIKE 'Acme%'
LIMIT 25;
```

## Analyst Workflow

Join Salesforce to local files:

```sql
SELECT a.Name, a.Industry, t.segment
FROM sf.Account a
JOIN read_csv_auto('territories.csv') t
  ON a.BillingState = t.state;
```

Materialize an analytical slice in DuckDB:

```sql
CREATE TABLE account_snapshot AS
SELECT Id, Name, Industry, CreatedDate
FROM sf.Account
WHERE CreatedDate >= DATE '2024-01-01';
```

Export to Parquet:

```sql
COPY account_snapshot TO 'account_snapshot.parquet' (FORMAT PARQUET);
```

Run server-side aggregates explicitly:

```sql
SELECT *
FROM salesforce_aggregate(
  'sf',
  'Account',
  'COUNT(Id) n, MIN(CreatedDate) first_created, MAX(CreatedDate) last_created',
  'IsDeleted = false');
```

Group in Salesforce, then continue analysis in DuckDB:

```sql
SELECT *
FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry');
```

## Transport

REST is the default scan path for smaller or interactive queries. Bulk is useful
for large extracts.

```sql
SET sf_force_transport = 'auto'; -- rest | bulk | auto
SET sf_bulk_chunks = 4;          -- opt-in PK chunking for Bulk

SELECT count(*) FROM sf.Account;
SELECT * FROM salesforce_query_cost();
SELECT * FROM salesforce_last_transport();
```

Important Bulk behavior:

- Bulk jobs complete server-side before result pages exist.
- Result CSV pages stream lazily after the job completes.
- `LIMIT` is not pushed into Bulk server-side, but lazy streaming stops
  downloading pages once DuckDB has enough rows.
- Blob/base64 body fields are not supported by Bulk API 2.0 CSV and are guarded
  before job creation.

## Relationships

Enable parent relationship traversal:

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;

SELECT Id, Account.Name, Account.Owner.Name
FROM sf.Contact
LIMIT 10;
```

Inspect what expanded and what was skipped:

```sql
SELECT *
FROM salesforce_relationships();
```

## Metadata

Salesforce describe metadata is available without using the Metadata API. A
shared per-catalog read-only Metadata Engine backs both the diagnostics and
Report Bridge.

```sql
-- Browse the org schema
SELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable;
SELECT field_name, type, filterable, reference_to, picklist_values
FROM salesforce_metadata_fields('sf', 'Account');

SELECT * FROM salesforce_picklist_values('sf', 'Account', 'Industry')
WHERE active;

SELECT * FROM salesforce_record_types('sf', 'Account');

SELECT * FROM salesforce_refresh_metadata('sf', 'Account');
```

Custom Metadata (`__mdt`) and queryable Custom Settings (`__c`) appear as normal
read-only sObject tables when visible to the authenticated user.

## Credential Sources

| `auth_source` | Use case |
|---|---|
| `options` | Local development with inline `ATTACH` options. |
| `env` | Terminals, Python jobs, notebooks, CI secrets. |
| `sfdx_url` | Reuse a Salesforce CLI auth URL (`SF_SFDX_AUTH_URL`). |
| `jwt` | Server-to-server jobs using a pre-authorized Connected App and RSA key. |

See [docs/en/function_manual.md](docs/en/function_manual.md) for exact options,
environment variables, and JWT requirements.

## Current Status

Own-repo release: **v0.14.2** (DuckDB v1.5.5 compatibility). Community
baseline: **v0.14.1**, merged in
[`duckdb/community-extensions#2078`](https://github.com/duckdb/community-extensions/pull/2078).
The previous upstream Windows CI blocker was cleared by the DuckDB `v1.5.4`
community pin; new DuckDB releases still require explicit validation.

| Area | Status |
|---|---|
| Native `ATTACH ... (TYPE salesforce)` | Done |
| REST read-only scans | Done |
| Bulk API 2.0 scans | Done |
| Lazy REST/Bulk streaming | Done |
| Auto REST/Bulk transport | Done |
| Bulk PK chunking + parallel execution | Done |
| API quota guard + query cost diagnostics | Done |
| `queryAll` archived/deleted reads | Done |
| Parent + grandparent relationship traversal | Done |
| Relationship diagnostics | Done |
| OAuth refresh-token / env / SFDX URL / JWT bearer | Done |
| Explicit server-side aggregates + `GROUP BY` | Done |
| Picklist and record type metadata helpers | Done |
| Manual metadata cache refresh | Done |
| Metadata Engine v2 + `metadata_objects` / `metadata_fields` | Done |
| Query explainability (`salesforce_query_explain()`) | Done |
| Linux + Windows + macOS arm64 CI | Done |
| Transparent `COUNT(field)` / `MIN` / `MAX` pushdown | Deferred: requires optimizer rewrite |
| Salesforce writes / Metadata API deploy | Out of scope |

## Build

```sh
git submodule update --init --recursive
make release
make test_release
```

Windows MSVC and RTOOLS/MinGW local validation are documented in
[docs/INSTALL.md](docs/INSTALL.md).

Validated matrix (official, from `MainDistributionPipeline.yml` run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085),
2026-08-28):

| Platform | DuckDB v1.5.2 | DuckDB v1.5.3 | DuckDB v1.5.4 | DuckDB v1.5.5 |
|---|---:|---:|---:|---:|
| Linux x64 | Pass | Pass | Pass | Pass |
| Windows x64 | **Fail** | **Fail** | Pass | Pass |
| macOS arm64 | Pass | Pass | Pass | Pass |
| Windows RTOOLS/MinGW local | - | Pass (historical, local) | - | - |

**v1.5.2/v1.5.3 currently fail on Windows in official CI.** The failure occurs
while compiling DuckDB's vendored `fmt` header with the current GitHub
Windows toolchain. The exact ownership — legacy DuckDB configuration, current
MSVC behavior, CI tooling, or their interaction — has not yet been isolated.
It is tracked as a separate legacy-compatibility issue and does not affect
the successful v1.5.4/v1.5.5 validation. A local Windows build of
v1.5.2/v1.5.3 previously passed (see git history) on the maintainer's machine
before this CI run existed as evidence — that result is now superseded by
this table for Windows on those two versions, per policy (GitHub Actions is
the official reference). Not investigated further in this delivery (out of
scope: this delivery targets v1.5.5 compatibility).

v1.5.5 was also validated locally on Windows x64 (Release + Debug, full
offline suite, 0 skipped) before this CI run — see `scripts/build_matrix.ps1`
and the `build(compat)` commit; kept as historical evidence, superseded by
the table above as the official result per current policy.

DuckDB extensions are version-locked to the DuckDB release used at build time.

## Documentation

Docs are split by language under `docs/en/` (English, primary) and `docs/pt/`
(Portuguese translations).

- [docs/en/usage_guide.md](docs/en/usage_guide.md) - analyst guide.
  (PT: [docs/pt/usage_guide.md](docs/pt/usage_guide.md))
- [docs/en/function_manual.md](docs/en/function_manual.md) - function and
  setting reference.
  (PT: [docs/pt/function_manual.md](docs/pt/function_manual.md))
- [docs/en/guide_windows.md](docs/en/guide_windows.md) - Windows build guide.
- [docs/en/guide_linux.md](docs/en/guide_linux.md) - Linux build guide.
- [docs/INSTALL.md](docs/INSTALL.md) - local installation and unsigned loading.
- [docs/ROADMAP.md](docs/ROADMAP.md) - bridge-first roadmap and deferred work.
- [docs/DOCS_PARITY.md](docs/DOCS_PARITY.md) - PT/EN documentation parity map.

## Repository Layout

```text
src/                  DuckDB extension implementation
src/include/          public/internal headers
test/sql/             offline sqllogictest suite
scripts/              smoke/build helpers
docs/en/, docs/pt/    public documentation
docs/community/       community descriptor and update history
third_party/httplib/  vendored HTTP client
```

## Governance

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- [SECURITY.md](SECURITY.md)
- [LICENSE](LICENSE)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## Community Catalog

This repository is published in the DuckDB community catalog. Publication still
does not happen automatically: future branch, push, PR, tag, or release actions
against `duckdb/community-extensions` require explicit human approval.

The live descriptor mirror lives at [docs/community/description.yml](docs/community/description.yml).
Readiness evidence lives at
[docs/community/PR_READINESS.md](docs/community/PR_READINESS.md).

Signed install path:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

## Author

**Fernando Lozer** - GitHub [@flozer](https://github.com/flozer) -
LinkedIn [/fernandolozer](https://www.linkedin.com/in/fernandolozer)

## License

MIT - see [LICENSE](LICENSE).

---

<div align="center">
  <h2>Support the project</h2>
  <p>If DuckDB Salesforce helps your work, you can support its continued development.</p>
  <a href="https://buymeacoffee.com/fernandolozer">
    <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50">
  </a>
</div>

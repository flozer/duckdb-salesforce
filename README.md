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
    <a href="https://github.com/flozer/duckdb-salesforce/releases/tag/v0.9.1"><img alt="release v0.9.1" src="https://img.shields.io/badge/release-v0.9.1-blue.svg"></a>
    <a href="https://github.com/flozer/duckdb-salesforce/actions/workflows/MainDistributionPipeline.yml"><img alt="Build + Test Linux Windows macOS" src="https://github.com/flozer/duckdb-salesforce/actions/workflows/MainDistributionPipeline.yml/badge.svg"></a>
    <img alt="DuckDB community ready" src="https://img.shields.io/badge/DuckDB%20community-ready-orange.svg">
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
- **Community-ready, human-gated** - docs, CI, license, security policy, and
  submission metadata are prepared; publication to `duckdb/community-extensions`
  requires explicit maintainer approval.

## Features

| Area | What works |
|---|---|
| Native DuckDB access | `ATTACH ... (TYPE salesforce)` exposes Salesforce objects as DuckDB tables. |
| Auth | OAuth refresh-token, SFDX auth URL, environment-based credentials, and JWT bearer. |
| REST scans | Lazy pagination, projection pushdown, predicate pushdown, `LIMIT` fetch reduction, `queryAll`. |
| Bulk scans | Bulk API 2.0 query, lazy result streaming, sequential/parallel PK chunking, quota guard. |
| Transport selection | `sf_force_transport = 'rest' | 'bulk' | 'auto'` with diagnostics explaining choices. |
| Relationships | Parent and grandparent traversal as nested `STRUCT`, with skip/expand diagnostics. |
| Metadata helpers | Refresh schema cache, inspect picklist values, record types, relationship expansion, query cost. |
| Aggregates | Transparent `COUNT(*)` pushdown and explicit `salesforce_aggregate()` with optional `GROUP BY`. |
| Diagnostics | Last SOQL, transport, quota, query cost, page counts, relationship decisions. |
| Testing | Offline mock suite plus validated live smoke; CI covers Linux, Windows, and macOS arm64. |

## Quick Start

Build locally or install the signed community artifact when it becomes available.
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

Salesforce describe metadata is available without using the Metadata API.

```sql
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

Release: **v0.9.1**.

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

Validated matrix:

| Platform | DuckDB v1.5.2 | DuckDB v1.5.3 |
|---|---:|---:|
| Linux x64 | Pass | Pass |
| Windows x64 | Pass | Pass |
| macOS arm64 | Pass | Pass |
| Windows RTOOLS/MinGW local | - | Pass |

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
docs/community/       community submission package, C.5-gated
third_party/httplib/  vendored HTTP client
```

## Governance

- [CONTRIBUTING.md](CONTRIBUTING.md)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- [SECURITY.md](SECURITY.md)
- [LICENSE](LICENSE)
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## Community Catalog

This repository is prepared for a DuckDB community extension submission, but no
publication happens automatically. Per project gate **C.5**, there is no branch,
push, PR, tag, or release to `duckdb/community-extensions` without explicit
human approval.

The staged descriptor lives at [docs/community/description.yml](docs/community/description.yml).
Readiness evidence lives at
[docs/community/PR_READINESS.md](docs/community/PR_READINESS.md).

After community acceptance, the expected signed install path is:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

Until then, use a local unsigned build.

## Author

**Fernando Lozer** - GitHub [@flozer](https://github.com/flozer) -
LinkedIn [/fernandolozer](https://www.linkedin.com/in/fernandolozer)

## License

MIT - see [LICENSE](LICENSE).

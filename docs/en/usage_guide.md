# duckdb-salesforce - Usage guide for analysts

This guide shows how to query a Salesforce org directly from DuckDB using
the `duckdb-salesforce` extension. The extension attaches your org as a
**read-only catalog**: Salesforce objects (sObjects) appear as tables you
can `SELECT` from with plain SQL, while the extension translates your query
into SOQL and the right Salesforce API (REST or Bulk) behind the scenes.

It is practical and example-driven. Every setting and function mentioned
here is real; nothing is invented.

## Official DuckDB references

This guide follows concepts documented by DuckDB:

- [`ATTACH`](https://duckdb.org/docs/current/sql/statements/attach.html):
  attaches another catalog to DuckDB. This extension uses the same model to
  expose a Salesforce org as a read-only catalog.
- [`SELECT`](https://duckdb.org/docs/stable/sql/statements/select):
  the only statement you run against the Salesforce catalog. All writes and
  DDL throw.
- [`CREATE TABLE`](https://duckdb.org/docs/stable/sql/statements/create_table):
  `CREATE TABLE ... AS SELECT` materializes a Salesforce query into a local
  DuckDB table.
- [Table functions](https://duckdb.org/docs/stable/sql/functions/overview):
  this extension ships diagnostic table functions such as
  `salesforce_query_cost()`.

## 1. Concepts before you start

### Read-only catalog

`ATTACH` exposes the org as a catalog. You can only read from it: every
`INSERT`, `UPDATE`, `DELETE`, and DDL statement against the Salesforce
catalog throws an error. If you need a local, writable copy, materialize a
query into a DuckDB table (see section 5).

### Tables are sObjects

Each table in the attached catalog is a Salesforce sObject (`Account`,
`Contact`, `Opportunity`, custom objects ending in `__c`, and so on).
Schema is resolved **lazily** the first time you reference an object, so
attaching is cheap and fast.

### Authentication and security

Authentication is **OAuth 2.0 refresh-token only**. You supply a
`client_id`, `client_secret`, and `refresh_token`. Credentials live in
memory only and are never logged. TLS certificate verification is always
on.

## 2. Installation and loading

A local build is **unsigned**, so DuckDB will not load it unless you opt in:

```sql
SET allow_unsigned_extensions=true;
LOAD 'path/to/salesforce.duckdb_extension';
```

For platform-specific build and install steps, see
[docs/en/guide_windows.md](./guide_windows.md) (Windows) and
[docs/en/guide_linux.md](./guide_linux.md) (Linux).

CI-validated platforms today are `linux_amd64` + `windows_amd64` (baseline) and
`osx_arm64` (extra). The extension is not yet published to community-extensions,
so there is no `INSTALL ... FROM community` yet.

### macOS: TLS certificate bundle

The extension always verifies the TLS server certificate when it talks to
Salesforce. There is no insecure flag, ever. On macOS the OpenSSL that the
extension is built against (via vcpkg) ships **no default CA bundle** and does
**not** read the macOS Keychain, so a *live* `ATTACH` can fail certificate
verification with no trust anchors to check against.

The fix is to point OpenSSL at a CA bundle through the `SSL_CERT_FILE`
environment variable (OpenSSL also honors `SSL_CERT_DIR` for a directory of
certificates). This only *selects* the trust anchors — verification stays ON,
it is not a bypass:

```bash
export SSL_CERT_FILE=$(brew --prefix)/etc/openssl@3/cert.pem   # Homebrew OpenSSL bundle
export SSL_CERT_FILE=$(python3 -m certifi)                     # Python certifi bundle
```

A live `ATTACH` that fails verification on macOS now prints this exact
suggestion in the error message. Linux and Windows need no such step: Linux
uses the system bundle and Windows uses the OS trust store. A zero-config macOS
Keychain trust store is a planned follow-up, not yet shipped.

## 3. Connect to Salesforce

Attach the org with the `salesforce` catalog type and your OAuth
credentials:

```sql
ATTACH 'salesforce://<org>' AS sf (
    TYPE salesforce,
    client_id 'your_consumer_key',
    client_secret 'your_consumer_secret',
    refresh_token 'your_refresh_token'
);
```

Two optional parameters cover sandboxes and pinned API versions:

```sql
ATTACH 'salesforce://<org>' AS sf (
    TYPE salesforce,
    client_id 'your_consumer_key',
    client_secret 'your_consumer_secret',
    refresh_token 'your_refresh_token',
    login_url 'https://login.salesforce.com',
    api_version 'v60.0'
);
```

Use `login_url 'https://test.salesforce.com'` for a sandbox. To obtain the
`client_id`, `client_secret`, and `refresh_token`, set up a Connected App
in your org and complete the OAuth flow as described in
[docs/CONNECTED_APP.md](../CONNECTED_APP.md).

### Auth sources

The `auth_source` `ATTACH` option chooses *where* the OAuth credentials come
from. The OAuth refresh-token flow is unchanged — only the source differs.
There are three modes:

- **`options`** (default): credentials are read from the inline `ATTACH`
  options `client_id`, `client_secret`, and `refresh_token` (all required),
  plus optional `login_url` and `api_version`. This is exactly the behavior
  shown above; you can omit `auth_source` entirely.
- **`env`**: credentials are read from environment variables. The required
  ones are `SF_CLIENT_ID`, `SF_CLIENT_SECRET`, and `SF_REFRESH_TOKEN`;
  `SF_LOGIN_URL` is optional. You do not pass the credential options in SQL.

  ```sql
  ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
  ```

- **`sfdx_url`**: credentials are read from a single environment variable,
  `SF_SFDX_AUTH_URL`, in the Salesforce CLI auth-URL format
  `force://<clientId>:<clientSecret>:<refreshToken>@<instance-host>` (the
  `<clientSecret>` segment may be empty). You do not pass the credential
  options in SQL.

  ```sql
  ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'sfdx_url');
  ```

The `api_version` option works in all three modes. With `env` and `sfdx_url`,
the credential options are not needed (the source wins).

#### Example: terminal

Export the variables in your shell, then start DuckDB and attach:

```bash
export SF_CLIENT_ID=your_consumer_key
export SF_CLIENT_SECRET=your_consumer_secret
export SF_REFRESH_TOKEN=your_refresh_token
# optional: export SF_LOGIN_URL=https://test.salesforce.com

duckdb
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

Or, using a Salesforce CLI auth URL captured from an authorized org:

```bash
export SF_SFDX_AUTH_URL=$(sf org display --verbose --json | jq -r .result.sfdxAuthUrl)

duckdb
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'sfdx_url');
```

#### Example: web UI / backend service

The application or container injects the `SF_*` environment variables (for
example, pulled from a secret manager at startup). The backend opens DuckDB
and attaches with `auth_source 'env'`, so no credentials ever appear in the
SQL text:

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

#### Example: Python / pipeline

```python
import os
import duckdb

os.environ["SF_CLIENT_ID"] = "your_consumer_key"
os.environ["SF_CLIENT_SECRET"] = "your_consumer_secret"
os.environ["SF_REFRESH_TOKEN"] = "your_refresh_token"

con = duckdb.connect()
con.execute(
    "ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env')"
)
```

#### Security note

Environment values and the SFDX auth URL are never logged, and tokens or
secrets never appear in error messages. A missing environment variable names
only the variable; a malformed `SF_SFDX_AUTH_URL` is reported as malformed
without echoing it; `invalid_grant` is reported as "refresh token is invalid,
expired, or revoked"; `invalid_client` as "client_id / client_secret is
incorrect".

#### Not supported in this cut

These are out of scope for this release: a browser / web OAuth flow, secret
storage, JWT Bearer, an OS credential manager, and token persistence.

## 4. First queries

Once attached, query sObjects like any other table:

```sql
SELECT Id, Name
FROM sf.Account
WHERE Name = 'Acme'
LIMIT 10;
```

Schema is resolved lazily on first reference. To list the objects you can
query, run a single global describe via:

```sql
SHOW TABLES;
-- or
SELECT * FROM duckdb_tables();
```

## 5. Materialize results locally

To keep a local, queryable snapshot (and to escape read-only limits), copy
a Salesforce query into a DuckDB table:

```sql
CREATE TABLE local_accounts AS
SELECT Id, Name, Industry, AnnualRevenue
FROM sf.Account
WHERE Industry = 'Manufacturing';
```

From then on you can query, join, and aggregate `local_accounts` with no
further API calls.

## 6. Transports: REST, Bulk, and auto

The extension can fetch rows over different Salesforce APIs. Choose with
`sf_force_transport`:

```sql
-- REST (default): lazy /query + queryMore paging
SET sf_force_transport='rest';

-- Bulk API 2.0: lazy-streamed result pages, optional PK chunking
SET sf_force_transport='bulk';

-- auto: probe row count, then pick rest or bulk
SET sf_force_transport='auto';
```

- **rest** (default) uses the REST `/query` + `queryMore` paging path,
  fetched lazily.
- **bulk** uses Bulk API 2.0. Result pages are streamed lazily, with
  optional PK chunking (section 7).
- **auto** runs one `SELECT COUNT()` to probe the row count, then picks
  rest or bulk based on `sf_auto_bulk_threshold` (default 50000). The probe
  is controlled by `sf_auto_probe` (default true).

Important about `LIMIT` with Bulk: a Bulk `LIMIT` is **not** server-side,
so the job runs fully. However, because result pages are streamed lazily, a
small `LIMIT` stops downloading later pages. Note that **auto cannot see
`LIMIT`** when probing, so for a small-`LIMIT` query against a huge object,
force `rest` explicitly:

```sql
SET sf_force_transport='rest';
SELECT Id, Name FROM sf.Account ORDER BY CreatedDate DESC LIMIT 20;
```

### COUNT pushdown

`COUNT(*)` and zero-column scans run a `SELECT COUNT()` (no record paging)
when there is no residual filter and the scan is not forced to bulk:

```sql
SELECT COUNT(*) FROM sf.Contact;
```

## 7. Large extractions: Bulk + PK chunking

For very large objects, Bulk API 2.0 plus PK chunking parallelizes the
extraction. `sf_bulk_chunks` (default 1 = off, cap 8, Bulk-only) splits a
Bulk scan into N disjoint `Id` ranges. The extension probes `MIN(Id)` /
`MAX(Id)`, performs a lexical split, and runs **one Bulk job per chunk** in
parallel (up to one thread per chunk):

```sql
SET sf_force_transport='bulk';
SET sf_bulk_chunks=8;

CREATE TABLE all_opps AS
SELECT Id, Name, Amount, StageName, CloseDate
FROM sf.Opportunity;
```

Caveats: there is **no global row order across chunks**, and chunks may be
uneven or even empty depending on how `Id` values distribute.

## 8. Quota governor

To avoid burning your daily API allotment, a quota governor gates **Bulk
job starts** (REST is not gated). Before starting a Bulk job, the extension
reads `GET /limits` and refuses the job when remaining requests are at or
below a threshold:

```
threshold = max(sf_quota_min_remaining, sf_quota_reserve_pct% of DailyApiRequests.Max)
```

Defaults: `sf_quota_min_remaining` is 1000 and `sf_quota_reserve_pct` is 10
(percent). Relevant settings:

```sql
SET sf_quota_enabled=true;       -- master switch (default true)
SET sf_quota_enforce=true;       -- false = warn only (default true)
SET sf_quota_fail_open=true;     -- /limits unavailable -> allow (default true)
SET sf_quota_cache_seconds=60;   -- cache /limits result (default 60)
SET sf_quota_min_remaining=1000; -- absolute floor (default 1000)
SET sf_quota_reserve_pct=10;     -- reserve % of daily max (default 10)
```

HTTP `429` responses are retried automatically;
`REQUEST_LIMIT_EXCEEDED` is terminal.

## 9. Relationships (opt-in)

By default, relationship columns are off. Set `sf_relationships='parent'`
to expose single-target parent lookups as `STRUCT` columns:

```sql
SET sf_relationships='parent';

SELECT Account.Name
FROM sf.Contact
LIMIT 10;
```

Scope and caveats:

- Depth is 1 (direct parent only) by default; raise `sf_relationship_depth`
  to `2` to also expand the grandparent.
- Polymorphic lookups are skipped.
- Predicates on subfields are evaluated as residual filters (in DuckDB).
- The default is `'off'`.

### Grandparent traversal (depth 2)

Set `sf_relationship_depth=2` to expand a single-target parent
relationship's own single-target parent (the grandparent) as a nested
`STRUCT` child. For example, Contact → Account → Owner (User):

```sql
SET sf_relationships='parent';
SET sf_relationship_depth=2;

SELECT Account.Owner.Name
FROM sf.Contact
LIMIT 10;
```

Scope and caveats:

- Depth is strictly capped at `2` (not Salesforce's full 5-level chains).
- Single-target only at each hop: polymorphic relationships are skipped at
  every level, and self-references or cycles are skipped.
- Predicates on subfields stay residual — there is no pushdown on
  `Account.Owner.Name` in `WHERE`, the same as at depth 1.
- Over-fetch grows with depth: selecting the STRUCT fetches all scalar
  fields at every expanded level.

## 10. Schema source

`sf_schema_source` controls how object schema is discovered:

```sql
-- describe (default): REST describe, authoritative
SET sf_schema_source='describe';

-- tooling: fast batched Tooling FieldDefinition
SET sf_schema_source='tooling';
```

- **describe** (default) uses the REST describe call and is authoritative.
- **tooling** uses the fast, batched Tooling `FieldDefinition` API, with a
  per-object REST fallback. It produces coarser types and reduces pushdown,
  so prefer it only when describe is too slow for your workflow.

## 11. Reading archived & deleted records (queryAll)

By default a scan returns only live records. Set `sf_query_mode='queryAll'`
to read via Salesforce's queryAll capability, which also returns archived
and soft-deleted records (`IsDeleted = true`) alongside live rows:

```sql
SET sf_query_mode='queryAll';

SELECT Id, Name, IsDeleted
FROM sf.Account
LIMIT 10;
```

Scope and caveats:

- Allowed values are `'query'` (default) and `'queryAll'`.
- The mode applies across the whole scan: REST uses the `/queryAll`
  endpoint, Bulk submits a job with `operation: "queryAll"`, and the
  `COUNT()`/`MIN`-`MAX(Id)` probes use it too — so COUNT pushdown, `auto`
  transport selection, and PK-chunk ranges all reflect deleted and archived
  rows.
- Use the `IsDeleted` column to tell deleted/archived rows apart from live
  ones.
- This is **not** history, CDC, or replication, and **not** a local
  snapshot — it only exposes the Salesforce read capability for that scan.
- The `salesforce_query()` utility is query-only and ignores this setting.

## 12. Diagnostics

The extension ships user-facing table functions that explain what the last
scan did. The richest is `salesforce_query_cost()`:

```sql
SELECT * FROM salesforce_query_cost();
```

It returns, among other columns: `object`, `soql`, `transport`,
`est_rows`, `transport_reason`, `projected_fields`, `total_fields`,
`pushed_filters`, `residual_filters`, `where_pushed`, `pages_fetched`,
`rows_emitted`, `bulk`, `count_pushdown`, `bulk_chunks`,
`quota_remaining`, `quota_allowed`, and `guidance`.

Focused helpers return a single aspect of the last scan:

```sql
SELECT * FROM salesforce_last_transport();    -- transport actually used
SELECT * FROM salesforce_last_quota();        -- quota snapshot
SELECT * FROM salesforce_last_soql();         -- SOQL that was sent
SELECT * FROM salesforce_last_scan_pages();   -- pages fetched
```

A typical workflow is to run your query, then inspect
`salesforce_query_cost()` to confirm the transport, the pushed vs. residual
filters, and the `guidance` column for tuning hints.

### Inspecting relationship expansion

`salesforce_relationships()` is a separate diagnostic that reports what
parent-relationship expansion did the **last time an object's schema was
resolved** (schema resolution happens on the first `SELECT` or `DESCRIBE` of
an object — re-querying a cached schema does not re-resolve). It complements
`salesforce_query_cost()`, which reports per-scan cost; this one reports
per-schema-resolution relationship expansion.

Resolve an object's schema first, then call it:

```sql
SET sf_relationships='parent';
SET sf_relationship_depth=2;
SELECT Id FROM sf.Contact LIMIT 1;   -- triggers schema resolution

SELECT row_type, relationship_name, parent_object, depth_level, status, reason, field_count
FROM salesforce_relationships();
```

The result always starts with exactly one `config` row (mode, effective
depth, and the `expanded_count` / `skipped_count` totals), followed by one
`relationship` row per `reference` field considered. Read it to answer four
questions:

- **Over-fetch.** An expanded parent is fetched as a **full STRUCT** of
  every queryable parent scalar field, and nested projection is not pushed
  into SOQL — so selecting `Account.Name` still fetches the whole parent.
  The `field_count` column (with the `note` on expanded rows) shows how wide
  that over-fetch is.
- **Skipped relationships.** A `relationship` row with `status='skipped'`
  carries a `reason` — `polymorphic` (for example a `What` lookup, with
  `parent_object` NULL), `self_reference`, `cycle`, `name_collision`,
  `parent_not_describable`, `no_fields`, or `no_relationship_name`.
- **Depth.** `depth_level=1` is the direct parent and `depth_level=2` is the
  grandparent (for example Contact → Account → Owner).
- **The always-present config row.** Even with `sf_relationships='off'` you
  still get the single config row (`relationships_mode='off'`) and no
  `relationship` rows — so "off" never looks like an empty or broken result:

```sql
SET sf_relationships='off';
SELECT Id FROM sf.Account LIMIT 1;

SELECT * FROM salesforce_relationships();   -- one config row only
```

## 13. Limitations

Know these boundaries before you build on the extension:

- **Read-only.** All writes and DDL against the Salesforce catalog throw.
- **Bulk `LIMIT` is not server-side**; the Bulk job runs fully (lazy
  streaming still stops later page downloads on a small `LIMIT`).
- **auto cannot see `LIMIT`** when probing. For a small-`LIMIT` query on a
  huge object, force `rest`.
- **Relationships are parent-only, depth 1–2** (`sf_relationship_depth`, capped
  at 2); polymorphic lookups are skipped and subfield predicates are residual.
- **Tooling schema source produces coarser types** and reduces pushdown.
- **Aggregate pushdown is COUNT-only.**
- **PK chunks have no global order**, and may be uneven or empty.
- **CI-validated platforms** are `linux_amd64` + `windows_amd64` (baseline) and
  `osx_arm64` (extra); `osx_amd64`, other ARM, and wasm are not validated yet.
  Live Salesforce TLS on macOS is unverified (use `SSL_CERT_FILE`).
- **Not yet on community-extensions**, and the project is **pre-1.0**.

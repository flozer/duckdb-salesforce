# Extension function manual

Public function and setting reference for `duckdb-salesforce`.

This is the English counterpart of `docs/pt/function_manual.md`. Keep both
files aligned when public behavior changes.

## How to read this manual

Use this document as a behavior reference, not as a product roadmap. Future
functions mentioned in the roadmap are not available until they are
implemented, tested, and documented here.

This manual covers every user-facing SQL surface the extension exposes:
the `salesforce` ATTACH storage type, session settings, diagnostic table
functions, and standalone utility functions. A clearly separated final
section documents debug/test-only entry points that are **not a stable
API**.

## Runtime requirement

The extension talks to Salesforce over the REST and Bulk APIs using an
OAuth refresh-token flow. You need a Connected App that issues a
`client_id`, `client_secret`, and a `refresh_token` for a user with read
access to the objects you query. See `docs/CONNECTED_APP.md` for how to
provision those credentials.

The attached catalog is **read-only**. The extension issues SOQL queries
and reads results; it never writes back to Salesforce.

## Level 1 - Attached org (storage type `salesforce`)

### `ATTACH 'salesforce://<org>' AS sf (TYPE salesforce, ...)`

Attaches a Salesforce org as a read-only DuckDB catalog. sObjects are
exposed as tables and resolved lazily — an sObject is described only when
it is first referenced, not at attach time.

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT Id, Name, AnnualRevenue
FROM sf.Account
WHERE BillingCountry = 'Brazil';
```

The `salesforce://<org>` string is a logical label for the attached org;
`<org>` is an arbitrary identifier you choose (for example `production` or
`sandbox`). It does not encode the instance host — the actual instance URL
is discovered from the OAuth token exchange.

Parameters:

| Parameter | Required | Default | Meaning |
|---|---|---|---|
| `TYPE salesforce` | yes | — | Selects this storage extension |
| `client_id` | yes | — | Connected App consumer key |
| `client_secret` | yes | — | Connected App consumer secret |
| `refresh_token` | yes | — | OAuth refresh token for the read user |
| `login_url` | no | `https://login.salesforce.com` | OAuth host; use the My Domain / sandbox host where applicable |
| `api_version` | no | extension default | Salesforce API version, e.g. `60.0` |

Behavior notes:

- **Read-only catalog.** No INSERT/UPDATE/DELETE/DDL against attached
  sObjects.
- **OAuth refresh-token flow.** The extension exchanges the refresh token
  for a short-lived access token and the instance URL; it refreshes as
  needed.
- **Lazy sObject resolution.** Schema for an sObject is fetched on first
  use, so attaching a large org is cheap.
- Use `login_url` to point at a sandbox (`https://test.salesforce.com`) or
  a My Domain login host.

### `information_schema` via `ATTACH`

After attaching, use the standard DuckDB catalog views to inspect exposed
sObjects and their columns:

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id 'KEY', client_secret 'SECRET', refresh_token 'TOKEN'
);

SELECT table_name
FROM information_schema.tables
WHERE table_catalog = 'sf';
```

## Level 2 - Session settings (`SET ...`)

These settings tune scan transport selection, the API-quota governor,
schema discovery, relationship expansion, and Bulk chunking. Each applies
to the current DuckDB session.

### Transport selection

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `sf_force_transport` | VARCHAR | `'rest'` | Scan transport: `'rest'`, `'bulk'`, or `'auto'` |
| `sf_auto_bulk_threshold` | BIGINT | `50000` | In `'auto'`: row count above which Bulk is chosen |
| `sf_auto_probe` | BOOLEAN | `true` | In `'auto'`: run the `COUNT()` probe to estimate rows; `false` => default to REST |

```sql
SET sf_force_transport = 'auto';
SET sf_auto_bulk_threshold = 100000;
```

When `sf_force_transport` is `'auto'`, the extension decides per scan: with
`sf_auto_probe = true` it runs a `COUNT()` probe and chooses Bulk when the
estimate exceeds `sf_auto_bulk_threshold`; with `sf_auto_probe = false` it
skips the probe and defaults to REST.

### API-quota governor

The governor consults the Salesforce `/limits` resource and the
`DailyApiRequests` limit before a scan, to avoid exhausting the org's daily
API allowance.

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `sf_quota_enabled` | BOOLEAN | `true` | Governor on; `false` skips the `/limits` consultation entirely |
| `sf_quota_enforce` | BOOLEAN | `true` | `false` = warn-only (consult `/limits` but never block) |
| `sf_quota_fail_open` | BOOLEAN | `true` | If `/limits` is unavailable, allow the scan; `false` = block |
| `sf_quota_reserve_pct` | BIGINT | `10` | Reserve this percent of `DailyApiRequests.Max` |
| `sf_quota_min_remaining` | BIGINT | `1000` | Absolute floor of remaining requests below which scans are blocked |
| `sf_quota_cache_seconds` | BIGINT | `60` | In-memory `/limits` TTL per `instance_url` (`0` = no cache) |

```sql
SET sf_quota_reserve_pct = 20;
SET sf_quota_min_remaining = 5000;
```

The governor blocks a scan when the projected remaining requests would fall
below either the reserve (`sf_quota_reserve_pct` of `DailyApiRequests.Max`)
or the absolute floor (`sf_quota_min_remaining`). With
`sf_quota_enforce = false` it still consults `/limits` and records the
decision but never blocks. With `sf_quota_fail_open = true` an unavailable
`/limits` endpoint allows the scan; set it to `false` to block when the
limit cannot be read.

### Schema discovery and relationships

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `sf_schema_source` | VARCHAR | `'describe'` | Schema discovery source: `'describe'` or `'tooling'` |
| `sf_relationships` | VARCHAR | `'off'` | `'off'`, or `'parent'` to expose parent relationships as STRUCT columns |

```sql
SET sf_schema_source = 'tooling';
SET sf_relationships = 'parent';
```

`sf_schema_source` selects which API supplies field metadata: the standard
Describe API (`'describe'`) or the Tooling API (`'tooling'`). With
`sf_relationships = 'parent'`, parent relationships are surfaced as nested
STRUCT columns alongside the flat fields.

### Bulk chunking

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `sf_bulk_chunks` | BIGINT | `1` | PK chunk count for Bulk scans (cap `8`, Bulk-only) |

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;
```

`sf_bulk_chunks` splits a Bulk scan into N primary-key ranges that are
fetched in parallel (one thread per chunk). It is capped at `8` and applies
only when the Bulk transport is used; it has no effect on REST scans.

## Level 3 - Diagnostics and observability

These are no-argument table functions that report on the **last scan** in
the current session. They are best-effort and reflect a single-thread
snapshot; under parallel Bulk execution they describe the coordinating
thread's view.

### `salesforce_query_cost()`

Returns one row describing the planning and execution of the most recent
scan: chosen transport and why, projection/filter pushdown counts, paging,
quota decision, and a human-readable guidance string.

```sql
SELECT * FROM sf.Account WHERE BillingCountry = 'Brazil';
SELECT * FROM salesforce_query_cost();
```

Output columns:

| Column | Type | Notes |
|---|---|---|
| `object` | VARCHAR | sObject scanned |
| `soql` | VARCHAR | SOQL sent to Salesforce |
| `transport` | VARCHAR | `rest` or `bulk` |
| `est_rows` | BIGINT | Estimated row count used for planning |
| `transport_reason` | VARCHAR | Why this transport was chosen |
| `projected_fields` | BIGINT | Fields requested in the SELECT |
| `total_fields` | BIGINT | Total fields on the sObject |
| `pushed_filters` | BIGINT | Predicates pushed into SOQL |
| `residual_filters` | BIGINT | Predicates kept and evaluated in DuckDB |
| `where_pushed` | VARCHAR | The WHERE clause actually pushed |
| `pages_fetched` | BIGINT | API result pages fetched |
| `rows_emitted` | BIGINT | Rows returned to DuckDB |
| `bulk` | BOOLEAN | Whether Bulk transport was used |
| `count_pushdown` | BOOLEAN | Whether a `COUNT()` pushdown was used |
| `bulk_chunks` | BIGINT | PK chunk count applied (Bulk) |
| `quota_remaining` | BIGINT | Remaining API requests at decision time |
| `quota_allowed` | BOOLEAN | Whether the quota governor allowed the scan |
| `guidance` | VARCHAR | Human-readable advice for tuning the scan |

Last-scan, best-effort, single-thread snapshot.

### `salesforce_last_transport()`

Returns the transport decision for the last scan.

| Column | Type | Notes |
|---|---|---|
| `transport` | VARCHAR | `rest` or `bulk` |
| `est_rows` | BIGINT | Estimated rows used in the decision |
| `reason` | VARCHAR | Why this transport was chosen |

### `salesforce_last_quota()`

Returns the quota-governor decision for the last scan.

| Column | Type | Notes |
|---|---|---|
| `limit_name` | VARCHAR | The Salesforce limit consulted (e.g. `DailyApiRequests`) |
| `max` | BIGINT | The limit maximum |
| `remaining` | BIGINT | Remaining requests at decision time |
| `threshold` | BIGINT | Effective block threshold (reserve / floor) |
| `allowed` | BOOLEAN | Whether the scan was allowed |
| `reason` | VARCHAR | Explanation of the decision |

### `salesforce_last_soql()`

Returns the SOQL string sent for the last scan.

| Column | Type | Notes |
|---|---|---|
| `soql` | VARCHAR | The SOQL query string |

### `salesforce_last_scan_pages()`

Returns the number of API result pages fetched during the last scan.

| Column | Type | Notes |
|---|---|---|
| `pages` | BIGINT | API result pages fetched |

## Level 4 - Utility / standalone functions

These functions take credentials as named arguments and do **not** require
an `ATTACH`. They are useful for one-off schema inspection or raw queries.

### `salesforce_describe(object, client_id := ..., ...)`

Describes the schema of a single sObject without attaching the org. Returns
one row per field.

```sql
SELECT *
FROM salesforce_describe(
  'Account',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

Arguments:

| Argument | Required | Meaning |
|---|---|---|
| `object` | yes | sObject API name to describe (positional) |
| `client_id :=` | yes | Connected App consumer key |
| `client_secret :=` | yes | Connected App consumer secret |
| `refresh_token :=` | yes | OAuth refresh token |
| `login_url :=` | no | OAuth host (default `https://login.salesforce.com`) |
| `api_version :=` | no | Salesforce API version |

Output columns (names approximate):

| Column | Type | Notes |
|---|---|---|
| `name` | VARCHAR | Field API name |
| `sf_type` | VARCHAR | Salesforce field type |
| `duckdb_type` | VARCHAR | Mapped DuckDB type |
| `nillable` | BOOLEAN | Whether the field accepts NULL |
| `length` | BIGINT | Declared length (text fields) |
| `precision` | BIGINT | Numeric precision |
| `scale` | BIGINT | Numeric scale |
| `filterable` | BOOLEAN | Whether the field can appear in a SOQL WHERE |
| `sortable` | BOOLEAN | Whether the field can appear in ORDER BY |

Use this to inspect an sObject's schema without attaching the whole org
catalog.

### `salesforce_query(soql, client_id := ..., ...)`

Runs a raw SOQL query and returns the result records as raw JSON record
strings, one per row. This is a low-level utility; no schema mapping or
pushdown planning is applied.

```sql
SELECT *
FROM salesforce_query(
  'SELECT Id, Name FROM Account LIMIT 10',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

Arguments mirror `salesforce_describe` (the first positional argument is the
SOQL string instead of an object name; the same credential named arguments
apply).

## Pushdown reference

The scan planner pushes as much of the query into SOQL as is safe, and
keeps anything else as a **residual** filter evaluated in DuckDB. A
residual filter never changes correctness — it only means more rows are
fetched than strictly necessary.

Pushed to SOQL:

- **Projection** — only the referenced fields are requested.
- **Comparisons** — `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Null tests** — `IS NULL`, `IS NOT NULL`.
- **`AND`** of pushable predicates.

Pushed as a superset prefilter, then kept as a residual (DuckDB
re-evaluates exactly):

- **`IN`** — pushed as a superset prefilter, kept residual.
- **`LIKE`** prefix / suffix / contains — pushed as a superset, kept
  residual.
- **`OR`** — pushed only when all children are themselves safe to push;
  pushed as a superset, kept residual.

Kept fully residual (not pushed):

- Function calls in predicates.
- `NOT`.
- Predicates on non-filterable fields.
- A WHERE clause that would exceed 4000 characters once rendered as SOQL.

Aggregates:

- `COUNT(*)` is pushed down as a SOQL `COUNT()`.
- Aggregates other than `COUNT(*)` are **not** pushed.

Use `salesforce_query_cost()` (columns `pushed_filters`, `residual_filters`,
`where_pushed`, `count_pushdown`) to see exactly what reached Salesforce for
a given scan.

## Debug / test-only functions

> **Not a stable API.** Everything in this section exists for the
> extension's own test suite and for low-level debugging. Names, arguments,
> output shapes, and existence may change without notice. Do not build
> production workflows on these.

- `salesforce_describe_calls()` — call-count instrumentation for Describe
  API requests.
- `salesforce_global_describe_calls()` — call-count instrumentation for
  Global Describe API requests.
- `salesforce_tooling_calls()` — call-count instrumentation for Tooling API
  requests.
- `salesforce_last_bulk_create_body()` — the request body of the most
  recent Bulk job create call.
- `salesforce_decode(fields_json, records_json)` — decodes raw Salesforce
  field/record JSON into typed rows; used to test the decode path in
  isolation.
- `sf_url_encode(s)` — URL-encodes a string; used to test query-string
  construction.

### Mock settings (offline test suite)

A family of `sf_mock_*` settings exists to drive the extension against
recorded fixtures for the offline test suite (no live org). These are
**not for production use** and are intentionally undocumented as a stable
surface; they exist only so the test harness can inject canned API
responses.

## Documentation premise

When public behavior changes:

- update this file
- update `docs/pt/function_manual.md`
- update usage guides when examples or workflows change
- update roadmap files when status changes

PT/EN docs must stay aligned in meaning, status, caveats, and examples.

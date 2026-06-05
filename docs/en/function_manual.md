# Extension function manual

Public function and setting reference for `duckdb-salesforce`.

This is the English counterpart of `docs/pt/function_manual.md`. Keep both
files aligned when public behavior changes.

## How to read this manual

This manual is written for a beginner. For each important function and
setting it follows the same four-part shape:

- **What it does** — a one-line plain-English explanation.
- **How it works** — the mechanics, plus the output columns or the
  type/default/allowed values.
- **Why use it** — when and why you reach for it.
- **Daily use** — a small, runnable SQL example.

Every example assumes you have already attached an org as `sf` (see the
ATTACH section) and uses the familiar `sf.Account` / `sf.Contact` tables.

Use this document as a behavior reference, not as a product roadmap. Future
functions mentioned in the roadmap are not available until they are
implemented, tested, and documented here.

A clearly separated final section documents debug/test-only entry points
that are **not a stable API**.

### Table of contents

- [Runtime requirement](#runtime-requirement)
- [Level 1 - Attached org](#level-1---attached-org-storage-type-salesforce)
- [Level 2 - Session settings](#level-2---session-settings-set-)
- [Level 3 - Diagnostics and observability](#level-3---diagnostics-and-observability)
- [Level 4 - Utility / standalone functions](#level-4---utility--standalone-functions)
- [Level 5 - Server-side aggregates](#level-5---server-side-aggregates)
- [Pushdown reference](#pushdown-reference)
- [Debug / test-only functions](#debug--test-only-functions)

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

#### What it does

Connects a Salesforce org to your DuckDB session as a read-only catalog
named `sf`. After this runs, each Salesforce sObject looks like a normal
table you can query — for example `sf.Account` or `sf.Contact`.

#### How it works

sObjects are resolved lazily: an sObject's schema is fetched only when you
first reference it, not at attach time, so attaching even a huge org is
cheap. Authentication uses the OAuth refresh-token flow — the extension
exchanges your refresh token for a short-lived access token and the
instance URL, and refreshes as needed. The catalog is **read-only**: no
INSERT/UPDATE/DELETE/DDL is allowed against attached sObjects.

The `salesforce://<org>` string is a logical label for the attached org;
`<org>` is an arbitrary identifier you choose (for example `production` or
`sandbox`). It does **not** encode the instance host — the actual instance
URL is discovered from the OAuth token exchange.

Parameters:

| Parameter | Required | Default | Meaning |
|---|---|---|---|
| `TYPE salesforce` | yes | — | Selects this storage extension |
| `auth_source` | no | `'options'` | Where credentials come from: `'options'`, `'env'`, `'sfdx_url'`, or `'jwt'` |
| `client_id` | with `'options'` | — | Connected App consumer key |
| `client_secret` | with `'options'` | — | Connected App consumer secret |
| `refresh_token` | with `'options'` | — | OAuth refresh token for the read user |
| `login_url` | no | `https://login.salesforce.com` | OAuth host; use the My Domain / sandbox host where applicable |
| `api_version` | no | extension default | Salesforce API version, e.g. `60.0` |

Use `login_url` to point at a sandbox (`https://test.salesforce.com`) or a
My Domain login host.

#### Choosing where credentials come from (`auth_source`)

The `auth_source` option selects where `ATTACH` reads the OAuth credentials
from. It does not change anything about the OAuth refresh-token flow itself —
only the place the `client_id`, `client_secret`, and `refresh_token` are
sourced from. The default is `'options'`, which is the inline behavior
described above.

- **`'options'`** (default): credentials are read from the inline `ATTACH`
  options `client_id`, `client_secret`, and `refresh_token` (all required),
  plus the optional `login_url` and `api_version`.

- **`'env'`**: credentials are read from environment variables instead of the
  SQL text. The required variables are `SF_CLIENT_ID`, `SF_CLIENT_SECRET`, and
  `SF_REFRESH_TOKEN`; `SF_LOGIN_URL` is optional. The inline credential
  options are not needed (the environment source wins).

  ```sql
  ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
  ```

- **`'sfdx_url'`**: credentials are read from a single environment variable,
  `SF_SFDX_AUTH_URL`, in the Salesforce CLI auth-URL format
  `force://<clientId>:<clientSecret>:<refreshToken>@<instance-host>` (the
  `<clientSecret>` segment may be empty). The inline credential options are
  not needed (the source wins).

  ```sql
  ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'sfdx_url');
  ```

- **`'jwt'`**: the OAuth 2.0 JWT bearer flow, for headless / CI / pipeline use.
  Instead of a refresh token, the extension RS256-signs a short-lived JWT
  assertion (`iss` = `client_id`, `sub` = `username`, `aud` = `login_url`,
  `exp` = now + 300s) and exchanges it at `<login_url>/services/oauth2/token`
  with `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer`. The returned
  access token is held in memory; there is **no** refresh token, so on a `401`
  the extension simply re-signs a fresh assertion. Required:

  - `client_id` — the JWT issuer (the Connected App consumer key).
  - `username` — the JWT subject, i.e. the Salesforce user to impersonate.
  - a private key, supplied **either** through the `SF_JWT_KEY_FILE`
    environment variable (recommended — keeps key paths out of the SQL text)
    **or** through the inline `private_key_file` `ATTACH` option (accepted for
    local-dev ergonomics only).

  `login_url` is optional (default `https://login.salesforce.com`; use
  `https://test.salesforce.com` for a sandbox) and `api_version` works as in
  the other modes. The key must be an **unencrypted** PKCS#1 or PKCS#8 PEM RSA
  key — encrypted keys are not supported.

  ```sql
  -- key path from the environment (recommended for pipelines)
  ATTACH 'salesforce://production' AS sf (
    TYPE salesforce,
    auth_source 'jwt',
    client_id   'YOUR_CONSUMER_KEY',
    username    'svc@example.com'
  );
  ```

  ```sql
  -- inline key path (local dev only; prefer SF_JWT_KEY_FILE in pipelines)
  ATTACH 'salesforce://production' AS sf (
    TYPE salesforce,
    auth_source      'jwt',
    client_id        'YOUR_CONSUMER_KEY',
    username         'svc@example.com',
    private_key_file '/path/to/server.key'
  );
  ```

  **Prerequisite:** the Connected App must be **pre-authorized** for this user
  — either admin-approved, or pre-authorized by the user through the digital
  certificate ("Use digital signatures") JWT setup. The token exchange fails
  otherwise.

The `api_version` option works in all four modes. With `'env'`, `'sfdx_url'`,
and `'jwt'` you do not pass the refresh-token credential options at all.

#### Security and error messages

Environment values and the SFDX auth URL are **never** logged, and tokens or
secrets **never** appear in error messages. The errors are written to be
actionable without leaking anything:

- A missing environment variable names **only** the variable (for example,
  `SF_REFRESH_TOKEN`), never its value.
- A malformed `SF_SFDX_AUTH_URL` reports that it is malformed without echoing
  the URL.
- `invalid_grant` from the refresh-token exchange is reported as "refresh
  token is invalid, expired, or revoked".
- `invalid_client` is reported as "client_id / client_secret is incorrect".

For `'jwt'`, the private key, the assembled JWT, and the signed assertion are
**never** logged and **never** appear in error messages:

- A missing key path names the option / environment variable that is expected
  (`private_key_file` or `SF_JWT_KEY_FILE`).
- A missing key file names **only** the path.
- A non-PEM, unparseable, or encrypted key is reported as such without echoing
  the key contents.
- `invalid_grant` from the JWT exchange is reported as "invalid, expired, or
  the user is not authorized for this Connected App".

#### Not supported in this cut

The following are explicitly out of scope for this release: a browser / web
OAuth flow, secret storage, encrypted private keys, an OS credential manager,
and token / key persistence.

#### Why use it

It is the entry point for everything else. Without an attached org you have
no `sf.Account` / `sf.Contact` tables to query, and none of the session
settings or diagnostics have anything to act on.

#### Daily use

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT Id, Name, AnnualRevenue
FROM sf.Account
WHERE Industry = 'Technology';
```

### Inspecting the catalog via `information_schema`

#### What it does

Lists the sObjects and columns the attached org exposes, using the standard
DuckDB catalog views — no special Salesforce function needed.

#### How it works

Once attached, the org's sObjects appear in the normal DuckDB catalog.
Filter `information_schema.tables` (and `.columns`) by the catalog name you
used in `ATTACH` (here, `sf`).

#### Why use it

To discover which tables and columns are available before you write a
query, or to confirm an sObject was resolved.

#### Daily use

```sql
SELECT table_name
FROM information_schema.tables
WHERE table_catalog = 'sf';
```

## Level 2 - Session settings (`SET ...`)

These settings tune scan transport selection, the API-quota governor,
schema discovery, relationship expansion, and Bulk chunking. Each `SET`
applies to the current DuckDB session.

### `sf_force_transport`

#### What it does

Chooses how a scan fetches rows from Salesforce: the REST API, the Bulk
API, or an automatic per-scan decision.

#### How it works

- Type: `VARCHAR`
- Default: `'rest'`
- Allowed values: `'rest'`, `'bulk'`, `'auto'`

With `'rest'` every scan uses the REST API. With `'bulk'` every scan uses
the Bulk API. With `'auto'` the extension decides per scan (see
`sf_auto_probe` and `sf_auto_bulk_threshold`).

#### Why use it

REST is best for small/medium result sets and low latency; Bulk is best for
very large extracts. Set this when you know the workload, or use `'auto'`
to let the planner pick.

#### Daily use

```sql
SET sf_force_transport = 'bulk';

SELECT Id, Name FROM sf.Account;
```

#### Blob/base64 guard

Bulk API 2.0 query results are CSV, which **cannot carry blob/base64
fields** (base64 Salesforce fields map to DuckDB `BLOB`). The extension
detects a projected base64 field from metadata alone — at any depth,
including inside a parent-relationship `STRUCT`:

- With `'auto'`, a projected base64 field keeps the scan on REST (it never
  picks Bulk); the reason is recorded as `auto: bulk-incompatible (projected
  base64 field 'NAME') -> rest`.
- With `'bulk'` (forced), the scan fails **before** any job is created:
  `projected base64 field 'NAME' is not supported by Bulk API 2.0 CSV; use
  'rest' or 'auto'`.
- With `'rest'`, behavior is unchanged (REST returns the field as base64).

If the base64 field is not projected, Bulk is allowed as usual. The chosen
transport and the reason appear in `salesforce_last_transport()` and
`salesforce_query_cost()`. The guard is metadata-driven and covers base64
only; other Bulk-unsupported objects are not pre-listed and surface as a
clear Salesforce Bulk-job error if forced.

### `sf_auto_bulk_threshold`

#### What it does

In `'auto'` mode, sets the estimated row count above which Bulk is chosen
instead of REST.

#### How it works

- Type: `BIGINT`
- Default: `50000`

Only consulted when `sf_force_transport = 'auto'`. If the estimated row
count for a scan exceeds this value, the planner chooses Bulk; otherwise it
chooses REST.

#### Why use it

To tune where the REST/Bulk boundary sits for your org and network — raise
it to keep more scans on REST, lower it to push more scans to Bulk.

#### Daily use

```sql
SET sf_force_transport = 'auto';
SET sf_auto_bulk_threshold = 100000;

SELECT Id, Name FROM sf.Account;
```

### `sf_auto_probe`

#### What it does

In `'auto'` mode, decides whether to run a `COUNT()` probe to estimate the
row count before choosing a transport.

#### How it works

- Type: `BOOLEAN`
- Default: `true`

When `true` and `sf_force_transport = 'auto'`, the extension runs a
`COUNT()` probe and compares the estimate against `sf_auto_bulk_threshold`.
When `false`, it skips the probe and simply defaults to REST.

#### Why use it

The probe costs one extra API call but gives an accurate transport
decision. Disable it (`false`) to save that call when you would rather
default to REST anyway.

#### Daily use

```sql
SET sf_force_transport = 'auto';
SET sf_auto_probe = false;   -- skip the COUNT() probe; default to REST

SELECT Id, Name FROM sf.Account;
```

### API-quota governor

The governor consults the Salesforce `/limits` resource and the
`DailyApiRequests` limit before a scan, to avoid exhausting the org's daily
API allowance. The settings below control it.

The governor blocks a scan when the projected remaining requests would fall
below either the reserve (`sf_quota_reserve_pct` of `DailyApiRequests.Max`)
or the absolute floor (`sf_quota_min_remaining`).

### `sf_quota_enabled`

#### What it does

Turns the API-quota governor on or off.

#### How it works

- Type: `BOOLEAN`
- Default: `true`

When `true`, the governor consults `/limits` before scans. When `false`,
the `/limits` consultation is skipped entirely and no quota check happens.

#### Why use it

Leave it on to protect the org's daily API budget. Turn it off only when
you are certain quota is not a concern and want to avoid the `/limits`
call.

#### Daily use

```sql
SET sf_quota_enabled = false;   -- no quota consultation at all

SELECT Id, Name FROM sf.Account;
```

### `sf_quota_enforce`

#### What it does

Chooses whether the governor actually blocks scans, or only warns.

#### How it works

- Type: `BOOLEAN`
- Default: `true`

When `true`, the governor can block a scan that would breach the limits.
When `false`, it still consults `/limits` and records the decision but
never blocks (warn-only).

#### Why use it

Use warn-only mode (`false`) to observe what the governor would do without
risking a blocked query while you tune the thresholds.

#### Daily use

```sql
SET sf_quota_enforce = false;   -- consult limits but never block

SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_quota();
```

### `sf_quota_fail_open`

#### What it does

Decides what happens when the `/limits` endpoint cannot be read.

#### How it works

- Type: `BOOLEAN`
- Default: `true`

When `true`, an unavailable `/limits` endpoint allows the scan to proceed
(fail open). When `false`, an unavailable `/limits` endpoint blocks the
scan (fail closed).

#### Why use it

Keep it `true` so a transient `/limits` outage does not break your queries;
set it `false` when you would rather refuse to run than risk overshooting
quota.

#### Daily use

```sql
SET sf_quota_fail_open = false;   -- block if /limits cannot be read

SELECT Id, Name FROM sf.Account;
```

### `sf_quota_reserve_pct`

#### What it does

Reserves a percentage of the daily API maximum as an untouchable buffer.

#### How it works

- Type: `BIGINT`
- Default: `10`

The governor blocks a scan whose projected remaining requests would drop
below this percent of `DailyApiRequests.Max`.

#### Why use it

To keep a safety margin of daily API calls for other systems and ad-hoc
work. Raise it to be more conservative.

#### Daily use

```sql
SET sf_quota_reserve_pct = 20;   -- keep 20% of the daily max in reserve

SELECT Id, Name FROM sf.Account;
```

### `sf_quota_min_remaining`

#### What it does

Sets an absolute floor of remaining API requests below which scans are
blocked.

#### How it works

- Type: `BIGINT`
- Default: `1000`

Independent of the percentage reserve: if projected remaining requests
would fall below this absolute number, the scan is blocked.

#### Why use it

For a hard, count-based guardrail that does not scale with the org's
maximum — useful when you want a fixed cushion.

#### Daily use

```sql
SET sf_quota_min_remaining = 5000;   -- never let remaining drop below 5000

SELECT Id, Name FROM sf.Account;
```

### `sf_quota_cache_seconds`

#### What it does

Caches the `/limits` response for a number of seconds to avoid re-fetching
it on every scan.

#### How it works

- Type: `BIGINT`
- Default: `60`

The `/limits` result is cached in memory per `instance_url` for this many
seconds. A value of `0` disables the cache (every scan re-reads `/limits`).

#### Why use it

A short cache cuts repeated `/limits` calls during a burst of queries.
Lower it (or set `0`) when you need the freshest possible remaining-quota
reading.

#### Daily use

```sql
SET sf_quota_cache_seconds = 0;   -- always read fresh /limits

SELECT Id, Name FROM sf.Account;
```

### `sf_schema_source`

#### What it does

Chooses which Salesforce API supplies field metadata when an sObject's
schema is resolved.

#### How it works

- Type: `VARCHAR`
- Default: `'describe'`
- Allowed values: `'describe'`, `'tooling'`

With `'describe'` the standard Describe API supplies field metadata; with
`'tooling'` the Tooling API does.

#### Why use it

Switch to `'tooling'` when you need metadata the standard Describe API does
not surface; otherwise leave it on `'describe'`.

#### Daily use

```sql
SET sf_schema_source = 'tooling';

SELECT Id, Name FROM sf.Account;
```

### `sf_relationships`

#### What it does

Controls whether parent relationships are exposed as nested STRUCT columns.

#### How it works

- Type: `VARCHAR`
- Default: `'off'`
- Allowed values: `'off'`, `'parent'`

With `'off'` only flat fields are exposed. With `'parent'`, parent
relationships are surfaced as nested STRUCT columns alongside the flat
fields. By default this is one level deep (the direct parent); to also
expand the grandparent, raise `sf_relationship_depth` to `2`.

#### Why use it

Enable `'parent'` when you want to read fields from a parent record (for
example a Contact's parent Account) without writing your own join.

#### Daily use

```sql
SET sf_relationships = 'parent';

SELECT Id, Name FROM sf.Contact;
```

### `sf_relationship_depth`

#### What it does

Controls how many levels of parent relationships are expanded as nested
STRUCT columns. Only meaningful when `sf_relationships = 'parent'`.

#### How it works

- Type: `BIGINT`
- Default: `1`
- Capped at `2`

With `1` (the default) only the direct parent is expanded — one level of
parent STRUCT columns, exactly the behavior described under
`sf_relationships`. With `2`, a single-target parent relationship's own
single-target parent (the **grandparent**) is also expanded, becoming a
nested STRUCT child of the parent STRUCT — for example Contact → Account →
Owner (User).

Expansion is single-target only at each hop: **polymorphic relationships
are skipped at every level**, and self-references or cycles are skipped.
The depth is strictly capped at `2` (it does not follow Salesforce's full
5-level relationship chains). Predicates on subfields stay **residual**
(there is no pushdown on `Account.Owner.Name` in `WHERE`), the same as at
depth 1. Over-fetch grows with depth: selecting the STRUCT fetches all
scalar fields at every expanded level — a documented trade-off, the same as
at depth 1. Grandparent expansion is describe-source only (the Tooling
schema stays flat, with no relationship expansion); the parent and
grandparent describes reuse the per-attach cache. On the wire, REST decodes
the nested JSON and Bulk decodes the nested CSV headers
(`Account.Owner.Name`).

#### Why use it

Raise it to `2` when you need a field two hops up the parent chain (for
example a Contact's Account's Owner) without writing your own join, while
keeping the read scoped and predictable.

#### Daily use

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;

SELECT Account.Owner.Name FROM sf.Contact LIMIT 10;
```

### `sf_query_mode`

#### What it does

Chooses the Salesforce read capability a scan uses, and so whether archived
and soft-deleted records are returned alongside live rows.

#### How it works

- Type: `VARCHAR`
- Default: `'query'`
- Allowed values: `'query'`, `'queryAll'`

With `'query'` (the default) a scan returns only live records — unchanged
behavior. With `'queryAll'` the scan reads via Salesforce's queryAll
capability, which **also** returns archived and soft-deleted records
(`IsDeleted = true`) in addition to live rows. The setting flows through
every part of a scan: the REST transport uses the `/queryAll` endpoint, the
Bulk transport submits a job with `operation: "queryAll"`, and the
`COUNT()`/`MIN`-`MAX(Id)` probes use it too — so `COUNT()` pushdown, `auto`
transport selection, and PK-chunk ranges all reflect deleted and archived
rows as well.

It is **not** history, CDC, or replication, and it is not a local snapshot:
it only exposes the Salesforce read capability for that scan. An invalid
value raises a clear `BinderException`
(`sf_query_mode must be 'query' or 'queryAll'`).

#### Why use it

Set `'queryAll'` when you need to see records that have been archived or
soft-deleted — for example to reconcile counts or audit deletions —
without leaving the normal table-scan path.

#### Daily use

```sql
SET sf_query_mode = 'queryAll';

SELECT Id, Name, IsDeleted FROM sf.Account LIMIT 10;
```

### `sf_bulk_chunks`

#### What it does

Splits a Bulk scan into several primary-key ranges fetched in parallel.

#### How it works

- Type: `BIGINT`
- Default: `1`
- Cap: `8` (Bulk-only)

`sf_bulk_chunks` splits a Bulk scan into N primary-key ranges that are
fetched in parallel (one thread per chunk). It is capped at `8` and applies
only when the Bulk transport is used; it has **no effect** on REST scans.

#### Why use it

To speed up a large Bulk extract by parallelizing it across PK ranges.

#### Daily use

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;

SELECT Id, Name FROM sf.Account;
```

## Level 3 - Diagnostics and observability

These are no-argument table functions that report on the **last scan** in
the current session. They are best-effort and reflect a single-thread
snapshot; under parallel Bulk execution they describe the coordinating
thread's view.

The pattern is always the same: run a query, then call the diagnostic
function to see what happened.

### `salesforce_query_cost()`

#### What it does

Gives a single, complete summary of how the most recent scan was planned
and executed — transport choice and reason, pushdown counts, paging, the
quota decision, and a human-readable tuning hint.

#### How it works

Returns one row describing the last scan. Output columns:

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
| `query_mode` | VARCHAR | The read mode used (`query` / `queryAll`) |
| `bulk_chunks` | BIGINT | PK chunk count applied (Bulk) |
| `quota_remaining` | BIGINT | Remaining API requests at decision time |
| `quota_allowed` | BOOLEAN | Whether the quota governor allowed the scan |
| `guidance` | VARCHAR | Human-readable advice for tuning the scan |

Last-scan, best-effort, single-thread snapshot.

#### Why use it

It is the one-stop diagnostic. When you want to understand or tune a scan,
this row tells you almost everything — which transport ran, how much was
pushed down, how many pages were fetched, and what to try next.

#### Daily use

```sql
SELECT * FROM sf.Account WHERE Industry = 'Technology';
SELECT * FROM salesforce_query_cost();
```

### `salesforce_relationships()`

#### What it does

Reports what parent-relationship expansion did the **last time a Salesforce
object's schema was resolved** — which parents were expanded as nested
STRUCT columns, which were skipped and why, and how much each expanded
parent over-fetches. It is a pure diagnostic with **zero behavior change**.

#### How it works

Schema resolution happens on the **first reference** to an object (a
`SELECT` or `DESCRIBE`). This function reflects the **most recently
resolved** object; re-querying an already-cached schema does not
re-resolve, so the report does not change until a *new* object's schema is
resolved.

It has a different lifecycle from `salesforce_query_cost()` — that function
reports scan cost (per scan), while this one reports schema-time
relationship expansion (per schema resolution). They are deliberately kept
as separate functions.

The output uses **one uniform schema** for two kinds of row, distinguished
by `row_type`:

- Exactly **one `config` row** is emitted first. It is **always** present —
  even when `sf_relationships = 'off'` — so an "off" result never looks
  empty or broken. When relationships are off, the config row is the
  **only** row.
- Then **one `relationship` row per `reference` field** considered during
  resolution.

Columns not relevant to a given row are `NULL`:

| Column | Type | `config` row | `relationship` row |
|---|---|---|---|
| `row_type` | VARCHAR | `'config'` | `'relationship'` |
| `object` | VARCHAR | resolved object | resolved object |
| `relationships_mode` | VARCHAR | `sf_relationships` value (`off` / `parent`) | NULL |
| `relationship_depth` | BIGINT | effective `sf_relationship_depth` (1..2) | NULL |
| `relationship_name` | VARCHAR | NULL | SF `relationshipName` (or field name) |
| `parent_object` | VARCHAR | NULL | target object; NULL if polymorphic |
| `depth_level` | BIGINT | NULL | 1 = parent, 2 = grandparent |
| `status` | VARCHAR | NULL | `'expanded'` or `'skipped'` |
| `reason` | VARCHAR | NULL | NULL if expanded; else a skip reason |
| `field_count` | BIGINT | NULL | # STRUCT fields when expanded; NULL if skipped |
| `expanded_count` | BIGINT | # expanded | NULL |
| `skipped_count` | BIGINT | # skipped | NULL |
| `note` | VARCHAR | summary | over-fetch note (expanded rows) |

A `relationship` row that was **skipped** carries one of these reasons in
`reason`:

| `reason` | Meaning |
|---|---|
| `polymorphic` | More than one possible target — no single STRUCT to build |
| `self_reference` | The field points back at the same object |
| `cycle` | The parent is already on the current expansion path |
| `name_collision` | The `relationshipName` clashes with an existing column |
| `parent_not_describable` | The parent describe failed |
| `no_fields` | The parent exposed no usable fields |
| `no_relationship_name` | The reference field has no usable relationship name |

#### Over-fetch (the key insight)

An expanded parent is fetched as a **full STRUCT** containing every
queryable parent scalar field — including the foreign-key id columns. The
`field_count` column reflects that full width. Nested projection is **not**
pushed into SOQL, so selecting a single subfield (for example
`Account.Name`) still fetches the **whole** parent STRUCT. The `note` on
each expanded row states this. Reading `field_count` together with that
note is how you spot — and quantify — over-fetch.

#### Why use it

To understand *why* a parent field did or did not appear as a nested STRUCT,
to see at a glance which relationships were skipped and for what reason, to
confirm the effective mode and depth, and to measure the over-fetch each
expanded parent adds. Because the config row is always present, you also get
an unambiguous answer when relationships are simply turned off.

#### Daily use

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;
SELECT Id FROM sf.Contact LIMIT 1;   -- triggers schema resolution

SELECT row_type, relationship_name, parent_object, depth_level, status, reason, field_count
FROM salesforce_relationships();
```

A sample read of that result:

- The **config row** (`row_type = 'config'`) shows
  `relationships_mode = 'parent'`, `relationship_depth = 2`, and the
  `expanded_count` / `skipped_count` totals with a one-line `note` summary.
- An **expanded** `Account` row (`depth_level = 1`, `status = 'expanded'`,
  `reason` NULL) with a `field_count` equal to the number of Account scalar
  fields, and a `note` explaining the full-STRUCT over-fetch.
- A **skipped** polymorphic row such as `What` (`status = 'skipped'`,
  `reason = 'polymorphic'`, `parent_object` NULL).
- A **grandparent** `Owner` row at `depth_level = 2`, expanded as a nested
  STRUCT child of its parent.

With `sf_relationships = 'off'`, the same call returns just the single
config row (`relationships_mode = 'off'`), and no `relationship` rows:

```sql
SET sf_relationships = 'off';
SELECT Id FROM sf.Account LIMIT 1;

SELECT * FROM salesforce_relationships();   -- one config row only
```

### `salesforce_last_soql()`

#### What it does

Shows the exact SOQL string the extension sent to Salesforce for the last
scan.

#### How it works

Returns one row. Output column:

| Column | Type | Notes |
|---|---|---|
| `soql` | VARCHAR | The SOQL query string |

#### Why use it

To see precisely how your DuckDB query was translated into SOQL, including
the projected fields and the pushed WHERE clause.

#### Daily use

```sql
SELECT Id, Name FROM sf.Account WHERE Industry = 'Technology';
SELECT * FROM salesforce_last_soql();
```

### `salesforce_last_transport()`

#### What it does

Reports which transport (REST or Bulk) the last scan used and why.

#### How it works

Returns one row. Output columns:

| Column | Type | Notes |
|---|---|---|
| `transport` | VARCHAR | `rest` or `bulk` |
| `est_rows` | BIGINT | Estimated rows used in the decision |
| `reason` | VARCHAR | Why this transport was chosen |

#### Why use it

To confirm whether `'auto'` mode (or your explicit setting) picked the
transport you expected, and to read the row estimate behind that choice.

#### Daily use

```sql
SET sf_force_transport = 'auto';
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_transport();
```

### `salesforce_last_quota()`

#### What it does

Reports the quota-governor's decision for the last scan.

#### How it works

Returns one row. Output columns:

| Column | Type | Notes |
|---|---|---|
| `limit_name` | VARCHAR | The Salesforce limit consulted (e.g. `DailyApiRequests`) |
| `max` | BIGINT | The limit maximum |
| `remaining` | BIGINT | Remaining requests at decision time |
| `threshold` | BIGINT | Effective block threshold (reserve / floor) |
| `allowed` | BOOLEAN | Whether the scan was allowed |
| `reason` | VARCHAR | Explanation of the decision |

#### Why use it

To see how close you are to the daily API limit and why the governor did or
did not allow a scan — invaluable when tuning the `sf_quota_*` settings.

#### Daily use

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_quota();
```

### `salesforce_last_scan_pages()`

#### What it does

Reports how many API result pages were fetched during the last scan.

#### How it works

Returns one row. Output column:

| Column | Type | Notes |
|---|---|---|
| `pages` | BIGINT | API result pages fetched |

#### Why use it

A high page count means many round trips. Use it to gauge whether tighter
filters, a smaller projection, or a different transport would reduce the
work.

#### Daily use

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_scan_pages();
```

## Level 4 - Utility / standalone functions

These functions take credentials as named arguments and do **not** require
an `ATTACH`. They are useful for one-off schema inspection or raw queries.

### `salesforce_describe(object, client_id := ..., ...)`

#### What it does

Describes the schema of a single sObject without attaching the whole org —
returns one row per field.

#### How it works

Pass the sObject name positionally and the credentials as named arguments.

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

#### Why use it

To inspect an sObject's fields and types without attaching the entire org
catalog — handy for quick metadata checks or scripts.

#### Daily use

```sql
SELECT *
FROM salesforce_describe(
  'Account',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

### `salesforce_query(soql, client_id := ..., ...)`

#### What it does

Runs a raw SOQL query and returns each result record as a raw JSON string,
one per row, without attaching the org.

#### How it works

This is a low-level utility: no schema mapping or pushdown planning is
applied. Arguments mirror `salesforce_describe`, except the first
positional argument is the SOQL string instead of an object name; the same
credential named arguments apply. It is query-only and does not honor
`sf_query_mode` (it never reads archived or soft-deleted records).

#### Why use it

For one-off raw SOQL — for example a quick lookup or a query that uses SOQL
features outside the normal table scan path.

#### Daily use

```sql
SELECT *
FROM salesforce_query(
  'SELECT Id, Name FROM Account LIMIT 10',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

### `salesforce_refresh_metadata(catalog [, object])`

#### What it does

Clears the in-memory metadata cache the connector keeps for an attached
Salesforce org, so the **next** reference re-fetches schema (and the object
listing) fresh from Salesforce. It is how you pick up org schema changes — a
new custom field, a new object — within a long-lived session **without** a
`DETACH` / `ATTACH`.

#### How it works

The connector caches metadata in memory **per `ATTACH`**: each object's schema
(resolved lazily on first reference), the global object listing, and the
parent-object describes used for relationship expansion. There is **no** data
cache and **no** on-disk cache — only this in-memory metadata.

This function clears that cache. It makes **no network call itself**; it only
drops the cached entries so the next reference re-fetches. The scope depends on
whether you pass `object`:

| Call | Scope | What is cleared | Effect on the next reference |
|---|---|---|---|
| `salesforce_refresh_metadata(catalog)` | global | the object listing **and** every resolved object schema (and parent describes) | the next listing scan re-fetches; the next reference to any object re-describes |
| `salesforce_refresh_metadata(catalog, object)` | object | **only** that object's resolved schema (and its parent-describe entry) | only that object re-describes; other objects and the object listing are left intact |

Arguments:

| Argument | Required | Meaning |
|---|---|---|
| `catalog` | yes | The `ATTACH` alias of an attached Salesforce org (for example `'sf'`). |
| `object` | no | An sObject name. Omit it for a global refresh; pass it to refresh just that object. |

It returns exactly **one** row:

| Column | Type | Notes |
|---|---|---|
| `catalog` | VARCHAR | The alias you passed |
| `scope` | VARCHAR | `'global'` (no `object`) or `'object'` (`object` given) |
| `object` | VARCHAR | The object name, or `NULL` for a global refresh |

#### Errors

The errors are clear and secret-free. The alias must be an attached Salesforce
catalog:

- If no catalog with that alias is attached: `no attached catalog named '<x>'`.
- If the alias names a catalog that is not a Salesforce catalog:
  `catalog '<x>' is not a Salesforce catalog`.

#### Why use it

To pick up org schema changes within a long-lived session without tearing down
the attachment. When someone adds a custom field to `Account` or creates a new
object, refresh the cache and the next query sees the change — no `DETACH` /
`ATTACH` round trip needed.

#### Daily use

```sql
-- after adding a field to Account in Salesforce:
SELECT * FROM salesforce_refresh_metadata('sf', 'Account');  -- re-describe Account on next query
-- or refresh everything (schemas + object listing):
SELECT * FROM salesforce_refresh_metadata('sf');
```

## Level 5 - Server-side aggregates

This is a single table function that runs a SOQL aggregate query for you and
returns the computed result, rather than dragging the underlying rows down
into DuckDB. It reuses an org you have already attached.

### `salesforce_aggregate(catalog, object, aggregates [, filter [, group_by]])`

#### What it does

Runs an explicit, opt-in server-side SOQL aggregate over an attached org and
returns exactly one row of the computed values. You choose to call it — it is
**not** a transparent pushdown of `MIN`/`MAX`/`SUM`/`AVG` on a normal table
scan.

#### How it works

All arguments are `VARCHAR` string literals:

| Argument | Required | Meaning |
|---|---|---|
| `catalog` | yes | The ATTACH alias of an already-attached Salesforce org (for example `'sf'`). The function **reuses** that catalog's authenticated session — no re-auth, and no credentials in the call. |
| `object` | yes | The sObject to aggregate (for example `'Account'`). Must be a valid identifier. |
| `aggregates` | yes | Comma-separated SOQL aggregate terms (see below). |
| `filter` | no | A SOQL `WHERE` body **without** the `WHERE` keyword (for example `Industry = 'Technology'`). |
| `group_by` | no | Comma-separated **simple** field identifiers to group by (for example `'Industry'` or `'Industry, Type'`). |

The `aggregates` argument is a comma-separated list of SOQL aggregate terms.
The allowed aggregate functions are `MIN`, `MAX`, `SUM`, `AVG`, `COUNT`, and
`COUNT_DISTINCT`. Each term may carry an alias in SOQL style — space-separated
after the function — for example `MIN(AnnualRevenue) minRev`.

The optional fifth argument, `group_by`, is a comma-separated list of **simple**
field identifiers (for example `'Industry'` or `'Industry, Type'`). It is
**positional after `filter`** — to group **without** a filter, pass an **empty
string** for `filter`:

```sql
salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry')
```

Only plain field names are accepted: no dotted / relationship fields, no
expressions, and no `ROLLUP` / `CUBE` / `HAVING` in this cut — those are
rejected with a clear error.

Internally it runs
`SELECT <group_by>, <aggregates> FROM <object> [WHERE <filter>] GROUP BY <group_by>`
over REST. Without `group_by` it runs `SELECT <aggregates> FROM <object>
[WHERE <filter>]` and returns exactly **one** row.

The return model:

- Without `group_by` the function returns exactly **one** row. With `group_by`
  it returns **one row per group** (multiple rows).
- When grouping, the **group columns come first**, each named by its field, then
  the aggregate columns follow.
- There is **one output column per aggregate term**.
- Every output column is typed `VARCHAR`. You cast in DuckDB (for example
  `CAST(n AS BIGINT)` or `CAST(minRev AS DECIMAL(18,2))`).
- An aggregate column is named by the term's alias when one is given; otherwise
  it is named `expr0`, `expr1`, ... in term order.

It honors `sf_query_mode` (`query` / `queryAll`), so the aggregate can include
or exclude archived and soft-deleted records the same way a table scan does.
The SOQL it sends is recorded in the diagnostics — see
`salesforce_last_soql()` and `salesforce_query_cost()`.

#### Validation and limitations

- **Every term must be an aggregate function.** Bare fields are rejected; this
  is what keeps the single-row contract.
- **`GROUP BY` is supported** via the optional `group_by` argument, but only
  for **simple field identifiers** — no dotted / relationship fields, no
  expressions. `ROLLUP`, `CUBE`, and `HAVING` remain out of scope and are
  rejected with a clear error. Without `group_by` the function still returns
  exactly one row.
- `;` and nested `SELECT` are rejected.
- The `object` must be a valid identifier, and the arguments are length-capped.
- Relationship / polymorphic aggregates are passed through to SOQL as written;
  any Salesforce error surfaces verbatim. Errors never contain secrets.

#### Not a transparent pushdown

Transparent `MIN`/`MAX`/`SUM`/`AVG` pushdown — rewriting an ordinary aggregate
query on `sf.Account` into server-side SOQL automatically — would require a
DuckDB `OptimizerExtension` and plan rewrite, which is deferred (see the
roadmap). This explicit function gives you the same benefit (the aggregation
happens on Salesforce, so no rows are dragged down into DuckDB) without that
optimizer machinery. There is no optimizer or plan rewrite involved: you opt in
by calling the function.

#### Why use it

When you want a server-side aggregate — a `MIN`/`MAX`/`SUM`/`AVG`/`COUNT` over a
large sObject — without fetching the underlying rows into DuckDB, and you are
willing to ask for it explicitly. It is the available stand-in for transparent
aggregate pushdown.

#### Daily use

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT
  CAST(minRev AS DECIMAL(18,2)) AS min_rev,
  CAST(maxRev AS DECIMAL(18,2)) AS max_rev,
  CAST(n AS BIGINT)             AS n
FROM salesforce_aggregate(
  'sf', 'Account',
  'MIN(AnnualRevenue) minRev, MAX(AnnualRevenue) maxRev, COUNT(Id) n',
  'Industry = ''Technology''');
```

Group by a field to get one row per group. To group **without** a filter, pass
an empty string for the filter:

```sql
SELECT Industry, CAST(n AS BIGINT) AS account_count
FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry')
ORDER BY account_count DESC;
```

Combine a non-empty filter with `group_by` to group only the matching rows —
for example
`salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', 'AnnualRevenue > 0', 'Industry')`.

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

> **DEBUG / TEST-ONLY — not a stable API.** Everything in this section
> exists for the extension's own test suite and for low-level debugging.
> Names, arguments, output shapes, and existence may change without notice.
> Do not build production workflows on these.

### `salesforce_describe_calls()`

> **DEBUG / TEST-ONLY — not a stable API.**

#### What it does

Reports a call count for Describe API requests — instrumentation used to
verify how often the extension hits the Describe API.

#### How it works

A no-argument table function returning the Describe call-count
instrumentation. Shape is internal and may change.

#### Why use it

Only when debugging or testing schema-resolution behavior; it lets a test
assert that describe calls were (or were not) made.

#### Daily use

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_describe_calls();
```

### `salesforce_global_describe_calls()`

> **DEBUG / TEST-ONLY — not a stable API.**

#### What it does

Reports a call count for Global Describe API requests.

#### How it works

A no-argument table function returning the Global Describe call-count
instrumentation. Shape is internal and may change.

#### Why use it

Only when debugging or testing how often the extension performs a global
describe.

#### Daily use

```sql
SELECT * FROM salesforce_global_describe_calls();
```

### `salesforce_tooling_calls()`

> **DEBUG / TEST-ONLY — not a stable API.**

#### What it does

Reports a call count for Tooling API requests.

#### How it works

A no-argument table function returning the Tooling call-count
instrumentation. Shape is internal and may change. Relevant when
`sf_schema_source = 'tooling'`.

#### Why use it

Only when debugging or testing Tooling-API-based schema discovery.

#### Daily use

```sql
SET sf_schema_source = 'tooling';
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_tooling_calls();
```

### `salesforce_last_bulk_create_body()`

> **DEBUG / TEST-ONLY — not a stable API.**

#### What it does

Returns the request body of the most recent Bulk job create call.

#### How it works

A no-argument table function exposing the raw body the extension last sent
to create a Bulk job. Shape is internal and may change.

#### Why use it

Only when debugging or testing Bulk job creation — for example to confirm
the SOQL or chunking embedded in the job request.

#### Daily use

```sql
SET sf_force_transport = 'bulk';
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_bulk_create_body();
```

### Other debug helpers

> **DEBUG / TEST-ONLY — not a stable API.**

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

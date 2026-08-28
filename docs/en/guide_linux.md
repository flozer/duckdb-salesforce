# duckdb-salesforce — Linux quick-start

`duckdb-salesforce` is an out-of-tree [DuckDB](https://github.com/duckdb/duckdb)
extension that lets you `ATTACH` a Salesforce org and query its objects as if
they were DuckDB tables. Unlike a traditional database connector, there is **no
client library to install** — the extension talks to Salesforce over HTTPS, so
the only runtime requirements are network reachability and a set of OAuth
credentials.

This guide is the local-dev build path on Linux. The first Linux build is
validated by the Ubuntu CI job (`.github/workflows`), which builds
`linux_amd64`, then runs the offline mock test suite — it never contacts
Salesforce and uses no secrets. The steps below are the local equivalent.

## Step 0 — Requirements

| Component | Notes | Install |
|---|---|---|
| Git | with submodule support | `apt install git` |
| gcc / g++ | C++17 | `apt install build-essential` |
| make | DuckDB extension harness | `apt install make` |
| CMake | >= 3.5 | `apt install cmake` |
| Ninja | optional, faster builds | `apt install ninja-build` |
| OpenSSL dev headers | TLS for the HTTPS transport | `apt install libssl-dev` |

OpenSSL provides the TLS layer for the HTTPS transport. In the standard build it
is also resolved through the [vcpkg](https://github.com/microsoft/vcpkg)
manifest (`vcpkg.json`); `libssl-dev` covers the case where you build against
the system OpenSSL. The HTTP client itself ([httplib](https://github.com/yhirose/cpp-httplib))
is vendored as a header-only dependency, so there is nothing extra to install
for it.

The extension is built and tested on Linux against DuckDB v1.5.2, v1.5.3,
v1.5.4, and v1.5.5 — all four pass on `linux_amd64` per the official CI run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085)
(2026-08-28). The extension is version-locked to the DuckDB release it was
built against, v1.5.2 through v1.5.5 inclusive.

## Step 1 — Clone and pin

```bash
git clone https://github.com/flozer/duckdb-salesforce.git
cd duckdb-salesforce

# Pull the duckdb + extension-ci-tools submodules and the vendored deps.
git submodule update --init --recursive
```

## Step 2 — Build

The project uses the standard [DuckDB extension harness](https://github.com/duckdb/extension-ci-tools)
Makefile. A release build is a single command:

```bash
make release
```

Two things land under `build/release/`:

- `build/release/duckdb` — a DuckDB shell with the `salesforce` extension
  **built in** (no `LOAD` needed). This is the authoritative binary for
  testing.
- `build/release/extension/salesforce/salesforce.duckdb_extension` — the
  loadable variant. Use this to `LOAD` into a stock DuckDB CLI of a matching
  version.

OpenSSL is pulled via the `vcpkg.json` manifest during configuration; if you
prefer the system OpenSSL, the `libssl-dev` package from Step 0 satisfies the
same dependency.

## Step 3 — Run the offline test suite

```bash
make test_release
```

The suite is **mock-only**: it exercises the extension against a local mock and
never reaches out to Salesforce, so it needs no org and no credentials. This is
the same suite CI runs on every push.

## Step 4 — Loading into a stock DuckDB CLI

If you already have a separately installed DuckDB CLI (of a matching version),
you can load the `.duckdb_extension` you just built without rebuilding the CLI.
Because the binary isn't signed by the DuckDB extension authority, you must
allow unsigned extensions:

```bash
duckdb -unsigned
```

```sql
SET allow_unsigned_extensions=true;
LOAD '/path/to/duckdb-salesforce/build/release/extension/salesforce/salesforce.duckdb_extension';
```

Alternatively, just use `build/release/duckdb`, which already has the extension
built in.

## Step 5 — ATTACH and query

Attach an org using an OAuth refresh token, then query its objects:

```sql
ATTACH 'salesforce' AS sf (
    TYPE salesforce,
    client_id    'YOUR_CONNECTED_APP_CLIENT_ID',
    client_secret 'YOUR_CONNECTED_APP_CLIENT_SECRET',
    refresh_token 'YOUR_OAUTH_REFRESH_TOKEN',
    login_url    'https://login.salesforce.com'
);

SELECT Id, Name FROM sf.Account LIMIT 10;
```

The extension exchanges the refresh token for a short-lived access token over
TLS and keeps it in memory for the session.

## Troubleshooting

### OpenSSL not found / vcpkg fails to provision it
Configuration resolves OpenSSL through the `vcpkg.json` manifest. If vcpkg
provisioning fails on your machine, install the system headers
(`apt install libssl-dev`) so CMake can find OpenSSL directly, then re-run
`make release`.

### `LOAD` fails with an unsigned-extension error
The extension is not signed by the DuckDB extension authority. Start the CLI
with `duckdb -unsigned`, or run `SET allow_unsigned_extensions=true;` before the
`LOAD`. Or skip `LOAD` entirely and use `build/release/duckdb`.

### `LOAD` fails with a version mismatch
The extension is version-locked to DuckDB v1.5.2 / v1.5.3 / v1.5.4 / v1.5.5. If
your stock CLI is a different version, the ABI won't match and `LOAD` errors
out. Match the CLI to
the version the extension was built against, or use the bundled
`build/release/duckdb`.

### TLS handshake or certificate errors on ATTACH
The HTTPS transport requires a working TLS stack. Confirm OpenSSL is present and
that the host can reach your Salesforce login endpoint
(`https://login.salesforce.com` or your My Domain URL).

## Security notes

- All Salesforce traffic uses TLS over HTTPS — there is no unencrypted
  transport.
- OAuth credentials (client id/secret, refresh token, derived access token) are
  held **in memory only** for the duration of the session; the extension does
  not persist them to disk.
- CI is **mock-only**: it never contacts Salesforce and uses no secrets, so
  builds and tests run with zero exposure of org credentials.

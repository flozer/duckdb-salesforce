# duckdb-salesforce — Windows quick-start

`duckdb-salesforce` is an out-of-tree [DuckDB](https://github.com/duckdb/duckdb)
extension that lets you `ATTACH` a Salesforce org and query its objects as if
they were DuckDB tables. Unlike a traditional database connector, there is **no
client library to install** — the extension talks to Salesforce over HTTPS, so
the only runtime requirements are network reachability and a set of OAuth
credentials.

This guide is the local-dev build path on Windows. CI builds `windows_amd64`
(alongside `linux_amd64`) and runs the offline mock test suite — it never
contacts Salesforce and uses no secrets.

## Step 0 — Requirements

| Component | Notes | Where to get it |
|---|---|---|
| Visual Studio Build Tools | Workload "Desktop development with C++" (MSVC + CMake + Ninja) | `winget install Microsoft.VisualStudio.2022.BuildTools` |
| vcpkg | manifest-mode dependency provisioning | <https://github.com/microsoft/vcpkg> |
| Git | with submodule support | `winget install Git.Git` |
| DuckDB CLI (optional, can use the one we build) | matching version | `winget install DuckDB.cli` |

OpenSSL provides the TLS layer for the HTTPS transport and is provisioned via
the [vcpkg](https://github.com/microsoft/vcpkg) manifest (`vcpkg.json`) using
the **`x64-windows-static`** triplet. The static triplet is required so the
extension links against the same `/MT` runtime DuckDB uses — a triplet mismatch
is the most common build failure on Windows. The HTTP client
([httplib](https://github.com/yhirose/cpp-httplib)) is vendored as a header-only
dependency, so there is nothing extra to install for it.

The extension is built and tested against DuckDB v1.5.2, v1.5.3, v1.5.4, and
v1.5.5, and is version-locked to those releases. No GNU `make` is required on
Windows.

**Current official CI status on Windows** (run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085),
2026-08-28): v1.5.4 and v1.5.5 build and test cleanly. **v1.5.2 and v1.5.3
currently fail.** The failure occurs while compiling DuckDB's vendored `fmt`
header with the current GitHub Windows toolchain. The exact ownership —
legacy DuckDB configuration, current MSVC behavior, CI tooling, or their
interaction — has not yet been isolated. It is tracked as a separate
legacy-compatibility issue and does not affect the successful v1.5.4/v1.5.5
validation. If you need v1.5.2/v1.5.3
on Windows today, expect to hit this same build failure.

## Step 1 — Clone and pin

```powershell
git clone https://github.com/flozer/duckdb-salesforce.git
cd duckdb-salesforce

# Pull the duckdb + extension-ci-tools submodules and the vendored deps.
git submodule update --init --recursive
```

## Step 2 — Build

From a **Visual Studio developer shell** (so MSVC, CMake, and Ninja are on
`PATH`), configure DuckDB with the vcpkg toolchain and the static triplet, then
build the Release configuration:

```powershell
cmake -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -B build/release

cmake --build build/release --config Release
```

Replace `<vcpkg>` with the path to your vcpkg checkout. Two things land under
`build\release\`:

- `build\release\duckdb` — a DuckDB shell with the `salesforce` extension
  **built in** (no `LOAD` needed). This is the authoritative binary for
  testing.
- `build\release\extension\salesforce\salesforce.duckdb_extension` — the
  loadable variant. Use this to `LOAD` into a stock DuckDB CLI of a matching
  version.

On Windows the system **ROOT certificate store** is used for TLS
automatically — there is no certificate bundle to configure.

## Step 3 — Loading into a stock DuckDB CLI

If you already have a separately installed DuckDB CLI (of a matching version),
you can load the `.duckdb_extension` you just built without rebuilding the CLI.
Because the binary isn't signed by the DuckDB extension authority, you must
allow unsigned extensions:

```powershell
duckdb -unsigned
```

```sql
SET allow_unsigned_extensions=true;
LOAD 'D:/path/to/duckdb-salesforce/build/release/extension/salesforce/salesforce.duckdb_extension';
```

Alternatively, just use `build\release\duckdb`, which already has the extension
built in.

## Step 4 — ATTACH and query

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
TLS (validated against the system ROOT store) and keeps it in memory for the
session.

## Troubleshooting

### Link errors / runtime mismatch (`/MT` vs `/MD`)
The vcpkg triplet must be `x64-windows-static` so the extension links against
the same `/MT` runtime DuckDB uses. If you configured with the default
`x64-windows` (dynamic) triplet, delete `build\release\CMakeCache.txt`,
re-configure with `-DVCPKG_TARGET_TRIPLET=x64-windows-static`, and rebuild.

### OpenSSL not found during configure
OpenSSL is provisioned via the `vcpkg.json` manifest. Make sure you passed
`-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` so vcpkg runs
in manifest mode and installs OpenSSL with the static triplet.

### `LOAD` fails with an unsigned-extension error
The extension is not signed by the DuckDB extension authority. Start the CLI
with `duckdb -unsigned`, or run `SET allow_unsigned_extensions=true;` before the
`LOAD`. Or skip `LOAD` entirely and use `build\release\duckdb`.

### `LOAD` fails with a version mismatch
The extension is version-locked to DuckDB v1.5.2 / v1.5.3 / v1.5.4 / v1.5.5. If
your stock CLI is a different version, the ABI won't match and `LOAD` errors
out. Match the CLI to
the version the extension was built against, or use the bundled
`build\release\duckdb`.

### TLS handshake or certificate errors on ATTACH
TLS validates against the Windows system ROOT certificate store. Confirm the
host can reach your Salesforce login endpoint (`https://login.salesforce.com` or
your My Domain URL) and that the ROOT store is current.

## Security notes

- All Salesforce traffic uses TLS over HTTPS, validated against the Windows
  system ROOT certificate store — there is no unencrypted transport.
- OAuth credentials (client id/secret, refresh token, derived access token) are
  held **in memory only** for the duration of the session; the extension does
  not persist them to disk.
- CI is **mock-only**: it never contacts Salesforce and uses no secrets, so
  builds and tests run with zero exposure of org credentials.

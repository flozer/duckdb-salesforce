# Installing `duckdb-salesforce` (local build)

`duckdb-salesforce` is an out-of-tree DuckDB extension. It is **not** published to
the DuckDB community-extensions repository (that step is human-gated — see
[PRE_COMMUNITY_CHECKLIST.md](PRE_COMMUNITY_CHECKLIST.md)). Until then, build it
locally and load the **unsigned** artifact.

## Supported DuckDB versions

Extensions are **version-locked** to the DuckDB they were built against. This
extension is built + tested against **DuckDB v1.5.2 and v1.5.3** (the CI matrix).
The pinned DuckDB + `extension-ci-tools` commits live in the git submodules; use
the matching DuckDB build/CLI for the artifact you produce.

## Prerequisites

Common: `git`, CMake ≥ 3.5, a C++17 compiler, OpenSSL.

- **Linux**: `gcc`/`g++`, `make`, `ninja-build`, `libssl-dev` (OpenSSL is also
  pulled via vcpkg by the standard build). First Linux build is validated by the
  Ubuntu CI job (see `.github/workflows/`).
- **Windows**: Visual Studio Build Tools (MSVC + CMake + Ninja) and vcpkg.
  OpenSSL is resolved via the vcpkg manifest (`vcpkg.json`), triplet
  `x64-windows-static` (must match DuckDB's `/MT` runtime).
- **macOS (Apple Silicon / `osx_arm64`)**: Xcode command-line tools + CMake;
  OpenSSL via vcpkg. The `osx_arm64` build + offline tests are validated by CI.
  ⚠️ **Live-TLS caveat**: OpenSSL-via-vcpkg does not read the macOS **Keychain**,
  so a *live* `ATTACH` may fail certificate verification. CI only runs the
  offline mock suite, so this is not exercised there. Until the macOS trust
  store is wired up, point OpenSSL at a CA bundle (e.g. `SSL_CERT_FILE=…`) for
  live use.

CI validates build + the offline (mock) test suite on **linux_amd64,
windows_amd64, and osx_arm64** across DuckDB v1.5.2 and v1.5.3.

## Build

```sh
git submodule update --init --recursive

# Standard DuckDB extension build (Linux/macOS):
make release            # or: make debug

# Run the offline (mock) test suite — never contacts Salesforce:
make test_release
```

On Windows without GNU `make`, configure DuckDB directly in a VS dev shell with
the vcpkg toolchain:

```
-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
-DVCPKG_TARGET_TRIPLET=x64-windows-static
```
then `cmake --build <build dir> --config Release`.

The build produces:

- `build/release/extension/salesforce/salesforce.duckdb_extension` — the
  **loadable** extension artifact (this is what you distribute/`LOAD`).
- `build/release/duckdb` — a DuckDB shell statically linked with the extension
  (handy for a quick check).

## Load the (unsigned) extension

A locally built artifact is **unsigned**, so DuckDB must be told to allow it:

```sql
SET allow_unsigned_extensions = true;          -- or CLI: duckdb -unsigned
LOAD '/path/to/salesforce.duckdb_extension';
```

(The statically linked `build/release/duckdb` shell already has it built in — no
`LOAD` needed there.)

## Quickstart (read-only)

```sql
-- Use a refresh-token OAuth Connected App (see docs/CONNECTED_APP.md).
ATTACH 'salesforce://myorg' AS sf (TYPE salesforce,
    client_id 'xxx', client_secret 'xxx', refresh_token 'xxx');

SELECT Id, Name FROM sf.Account WHERE Name = 'Acme' LIMIT 10;
```

See the [README](../README.md) for transport (`rest`/`bulk`/`auto`), quota
governor, COUNT pushdown, relationships, Tooling schema, and PK chunking.

## Security notes

- Credentials live only in memory for the session; they are never logged.
- TLS server-certificate verification is always on (no insecure build flag).
- The test suite is mock-only and requires no secrets; CI never contacts
  Salesforce.

# Installing `duckdb-salesforce` (local build)

`duckdb-salesforce` is an out-of-tree DuckDB extension published in the DuckDB
Community Extensions registry. Install and load the signed build with:

```sql
INSTALL salesforce FROM community;
LOAD salesforce;
```

For development and local testing, build from source and load the **unsigned**
artifact as described below. Community updates remain human-gated; see
[PRE_COMMUNITY_CHECKLIST.md](PRE_COMMUNITY_CHECKLIST.md).

## Supported DuckDB versions

Extensions are **version-locked** to the DuckDB they were built against. This
extension is built + tested against **DuckDB v1.5.2, v1.5.3, v1.5.4, and
v1.5.5**, per the official `MainDistributionPipeline.yml` run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085)
(2026-08-28): v1.5.4 and v1.5.5 pass on Linux x64, Windows x64, and macOS
arm64. **v1.5.2 and v1.5.3 currently fail on Windows x64** in that same run.
The failure occurs while compiling DuckDB's vendored `fmt` header with the
current GitHub Windows toolchain. The exact ownership — legacy DuckDB
configuration, current MSVC behavior, CI tooling, or their interaction — has
not yet been isolated. It is tracked as a separate legacy-compatibility issue
and does not affect the successful v1.5.4/v1.5.5 validation.
v1.5.2/v1.5.3 still pass on Linux x64 and macOS arm64. See README.md's
validated matrix table for the full per-platform breakdown. A prior local-only
Windows result (this same maintainer's machine, before this CI run existed)
is kept only as historical evidence and does not override the table above.
The pinned DuckDB + `extension-ci-tools` commits live in the git submodules;
use the matching DuckDB build/CLI for the artifact you produce.

DuckDB v2.0 readiness (ahead of any stable v2.0 release) is tracked separately
in [docs/superpowers/specs/2026-08-27-duckdb-v2-c-api-readiness.md](superpowers/specs/2026-08-27-duckdb-v2-c-api-readiness.md);
no v2.0 support is claimed until a stable v2.0 tag exists and passes this same
local matrix.

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
  store is wired up (planned follow-up), point OpenSSL at a CA bundle via the
  `SSL_CERT_FILE` environment variable — OpenSSL reads it (and `SSL_CERT_DIR`)
  at runtime, and verification stays **on**:

  ```sh
  # Homebrew OpenSSL bundle:
  export SSL_CERT_FILE=$(brew --prefix)/etc/openssl@3/cert.pem
  # …or the Python certifi bundle:
  export SSL_CERT_FILE=$(python3 -m certifi)
  ```

  A live `ATTACH` that fails verification on macOS now prints this exact
  suggestion in the error message.

CI validates build + the offline (mock) test suite on **linux_amd64,
windows_amd64, and osx_arm64** across DuckDB v1.5.2, v1.5.3, v1.5.4, and
v1.5.5 — run
[33175570085](https://github.com/flozer/duckdb-salesforce/actions/runs/33175570085),
2026-08-28. All four versions pass on linux_amd64 and osx_arm64. On
windows_amd64, v1.5.4 and v1.5.5 pass; v1.5.2 and v1.5.3 fail while compiling
DuckDB's vendored `fmt` header — root cause not yet isolated, see the
"Supported DuckDB versions" section above for the full statement.

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

For a local MinGW/RTOOLS validation on Windows, use the dedicated script:

```powershell
pwsh -File scripts/build_rtools_local.ps1
```

This path uses RTOOLS compilers plus vcpkg `x64-mingw-static` for OpenSSL 3.
Do not rely on the OpenSSL bundled with RTOOLS itself; older RTOOLS installs can
provide OpenSSL 1.1.x, which is too old for the vendored `httplib`.

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

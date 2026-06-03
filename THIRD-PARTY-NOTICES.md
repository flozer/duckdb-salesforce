# Third-Party Notices

`duckdb-salesforce` (MIT — see [LICENSE](LICENSE)) bundles or links the following
third-party components. Each remains under its own license.

## cpp-httplib (vendored)

- Path: `third_party/httplib/httplib.h`
- License: **MIT**
- Copyright (c) 2024 Yuji Hirose
- <https://github.com/yhirose/cpp-httplib>

Header-only HTTP/HTTPS client used for the Salesforce transport.

## OpenSSL (linked, via vcpkg)

- License: **Apache License 2.0**
- <https://www.openssl.org/>

Provides TLS for the HTTPS transport (certificate verification always on).

## DuckDB and build tooling

- **DuckDB** (`duckdb/duckdb`) — MIT.
- **extension-ci-tools** (`duckdb/extension-ci-tools`) — MIT.
- **vcpkg** (Microsoft) — MIT.

DuckDB and all build/CI dependencies are governed by their own respective
licenses; this notice does not modify them.

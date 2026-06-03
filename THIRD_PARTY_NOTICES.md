# Third-party notices

`duckdb-salesforce` itself is licensed under the **MIT License** (`LICENSE`).
The source tree additionally uses the third-party material listed below. Their
original copyright headers and license terms are preserved inside the
corresponding files.

The HTTPS transport is built from vendored, header-only `httplib` plus
**OpenSSL** (linked, resolved via the `vcpkg.json` manifest). No Salesforce
client library exists or is bundled — the connector speaks the REST and Bulk
APIs directly over HTTPS.

## cpp-httplib — MIT

| Source | Version | License | Where in this repo |
|---|---|---|---|
| `cpp-httplib` (header-only HTTP/HTTPS client) | 0.18.3 | MIT | `third_party/httplib/httplib.h` |

Copyright (c) 2024 Yuji Hirose. Upstream:
<https://github.com/yhirose/cpp-httplib>. Used at compile time for the
Salesforce HTTPS transport; OpenSSL is enabled via `CPPHTTPLIB_OPENSSL_SUPPORT`.

## OpenSSL — Apache License 2.0

| Source | License | How |
|---|---|---|
| OpenSSL (TLS) | Apache-2.0 | linked, via the `vcpkg.json` manifest (triplet `x64-windows-static` on Windows) |

Provides TLS for the HTTPS transport; server-certificate verification is always
on. Upstream: <https://www.openssl.org/>.

## DuckDB and build tooling — MIT

The build pulls DuckDB's headers and the extension build harness via the
`duckdb` and `extension-ci-tools` submodules, with `vcpkg` (Microsoft) for
dependencies. DuckDB (`duckdb/duckdb`), `extension-ci-tools`
(`duckdb/extension-ci-tools`), and `vcpkg` are each distributed under the MIT
License; refer to their repositories for the authoritative `LICENSE` files.
This notice does not modify them.

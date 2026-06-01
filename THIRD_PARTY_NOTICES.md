# Third-Party Notices

This project bundles or links the following third-party components.

## cpp-httplib (vendored)

- Path: `third_party/httplib/httplib.h`
- Version: 0.18.3
- Upstream: https://github.com/yhirose/cpp-httplib
- License: MIT

Used for the live HTTPS transport. Header-only; vendored verbatim.

## OpenSSL (linked via vcpkg)

- Upstream: https://www.openssl.org/
- License: Apache License 2.0
- Pulled at build time via vcpkg (`vcpkg.json`), triplet `x64-windows-static`
  on Windows. Provides TLS for the HTTPS transport (cpp-httplib uses OpenSSL
  when `CPPHTTPLIB_OPENSSL_SUPPORT` is defined).

## DuckDB / extension-ci-tools (submodules)

- `duckdb` and `extension-ci-tools` are git submodules, used as the extension
  build host. See their respective repositories for license terms.

# Security Policy

## Status & scope

`duckdb-salesforce` is at **v0.8 — read-only, feature-complete for local use;
not yet published to community-extensions.** Live validation is **manual-only**
and must be run only against an org the maintainer is **authorized to use**;
automated CI must never contact Salesforce or require secrets. Treat it as
pre-1.0 until a versioned release explicitly states production readiness.

This policy is the enforced, reviewable form of the security gate in
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) Appendix C.4. It is a **hard
merge blocker**: no functional authentication or transport code (issues #3,
#4, and later) merges to `main` unless it satisfies every rule below.

## Secret handling (hard rules)

The two sensitive credentials are `client_secret` and `refresh_token`
(and, once issue #3 lands, the derived `access_token`).

1. **Never logged or echoed.** Secrets must not appear in log output, error
   messages, exception text, query plans, `EXPLAIN`, telemetry, or test
   fixtures. Error messages report only the *presence/absence* of an option
   (its key), never its value. (Implemented for parsing in
   `src/salesforce_config.cpp`.)
2. **Log masking.** When HTTP tracing/debug output is added (#4),
   `Authorization: Bearer …` headers and any token-bearing request/response
   fields must be redacted before they reach any sink.
3. **In-memory only.** Tokens are held in process memory for the lifetime of
   the ATTACH and never written to disk. No on-disk token cache, no plaintext
   in the catalog or metadata cache, until a separate, reviewed secure-storage
   design exists.
4. **Refresh-token / JWT only.** Only the OAuth 2.0 refresh-token flow (and,
   later, the JWT bearer flow) are permitted. The username-password OAuth flow
   and any handling of a Salesforce account password are **forbidden**.
5. **TLS always on.** All Salesforce traffic uses HTTPS with certificate
   verification enabled. No `verify=false` / insecure-TLS escape hatch ships
   in any build.

## Credential hygiene for users

- Use a dedicated **Connected App** scoped to the minimum required OAuth
  scopes; prefer a sandbox.
- Pass `client_secret` / `refresh_token` via `ATTACH` options from a secrets
  manager or environment indirection — avoid committing them to SQL scripts,
  notebooks, or version control.
- Rotate / revoke the refresh token if it may have been exposed.

## Community publication gate

Per `docs/ARCHITECTURE.md` Appendix C.5, **no** push, PR, tag, or release to
`duckdb/community-extensions` happens without explicit human go/no-go, after
multi-test evidence. The default flow stays in `flozer/duckdb-salesforce`.

## Reporting a vulnerability

This is an early-stage personal project. Report suspected security issues
privately to the repository owner via a **GitHub private security advisory**
(Security → Advisories) rather than a public issue. Please do not include live
credentials in a report.

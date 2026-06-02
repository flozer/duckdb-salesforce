# Manual Sandbox Smoke Test (v0.1)

Repeatable, **manual** validation of `duckdb-salesforce` against a real
Salesforce org. This is **never** run in CI and is **never** automated. It
exists to validate a build before tagging `v0.1.0`.

> **Manual-only.** Run this only against an org the maintainer is **authorized
> to use** (a sandbox/scratch/Developer Edition, or your own org). Automated CI
> must never contact Salesforce or require secrets. You run these steps
> yourself and control the credentials.

## 0. Security rules (read first)

- **Never** paste `client_secret`, `refresh_token`, or any `access_token` into
  an issue, PR, commit, log, screenshot, or this evidence file.
- Pass secrets via environment variables only (below). Do not commit them.
- The connector never logs the request body or tokens; keep it that way — do
  not add debug prints of headers/bodies during smoke.
- If a token may have been exposed, **revoke/rotate it** in the org immediately.
- You may redact `instance_url` in shared evidence if you consider it sensitive.
- **C.5 gate:** a passing smoke test does NOT authorize any push, PR, tag, or
  release to `duckdb/community-extensions`. That remains blocked pending a
  separate explicit human go/no-go (after Win/Linux matrix + package review).

## 1. Prerequisites

- An org you are **authorized to use** (sandbox / scratch / Developer Edition,
  or your own).
- A **Connected App** with OAuth enabled, the minimum scopes (`api`,
  `refresh_token`), and a redirect URI.
- A **refresh token** obtained via the OAuth 2.0 device or web-server flow —
  step-by-step in [docs/CONNECTED_APP.md](docs/CONNECTED_APP.md). Treat it as a
  secret. (Username+password is not supported — refresh-token flow only.)
- A local release build (see [README → Build](README.md#build)).

## 2. Environment variables

| Variable | Required | Meaning |
| --- | --- | --- |
| `SF_LIVE_CLIENT_ID` | yes | Connected App consumer key |
| `SF_LIVE_CLIENT_SECRET` | yes | Connected App consumer secret |
| `SF_LIVE_REFRESH_TOKEN` | yes | OAuth refresh token for the org |
| `SF_LIVE_LOGIN_URL` | no | `https://login.salesforce.com` (default / Dev/scratch) or `https://test.salesforce.com` (sandbox) |

PowerShell:

```powershell
$env:SF_LIVE_CLIENT_ID     = '...'
$env:SF_LIVE_CLIENT_SECRET = '...'
$env:SF_LIVE_REFRESH_TOKEN = '...'
# sandbox only:
$env:SF_LIVE_LOGIN_URL     = 'https://test.salesforce.com'
```

bash:

```sh
export SF_LIVE_CLIENT_ID='...' SF_LIVE_CLIENT_SECRET='...' SF_LIVE_REFRESH_TOKEN='...'
# sandbox only:
export SF_LIVE_LOGIN_URL='https://test.salesforce.com'
```

To clear them afterwards: `Remove-Item Env:SF_LIVE_*` (PowerShell) /
`unset SF_LIVE_CLIENT_ID SF_LIVE_CLIENT_SECRET SF_LIVE_REFRESH_TOKEN SF_LIVE_LOGIN_URL` (bash).

## 3. Run the gated live tests

The `*_live.test` files are skipped unless the three required env vars are set.
They currently ATTACH with the **default** login host (`login.salesforce.com`);
for a sandbox that needs `test.salesforce.com`, use the ad-hoc run in §4 instead
(or run these against a Dev/scratch org).

```sh
build/release/test/unittest "test/sql/salesforce_oauth_live.test"
build/release/test/unittest "test/sql/salesforce_describe_live.test"
build/release/test/unittest "test/sql/salesforce_query_live.test"
build/release/test/unittest "test/sql/salesforce_scan_live.test"
```

Expected: each prints `All tests passed` (or `Skipped` if env not set — which
means the smoke did NOT run).

## 4. Ad-hoc smoke via the DuckDB shell (works for sandbox via login_url)

Use this for a sandbox (sets `login_url` to `test.salesforce.com`), which the
gated test files do not. Paste the secret values **only in a local interactive
session**; do not save the command to a file, do not commit it, and clear your
shell history afterwards. (DuckDB has no env-var function, so the values are
typed inline here.)

```sh
build/release/duckdb
```

```sql
LOAD salesforce;

ATTACH 'salesforce://smoke' AS sf (TYPE salesforce,
    client_id     '<SF_LIVE_CLIENT_ID>',
    client_secret '<SF_LIVE_CLIENT_SECRET>',
    refresh_token '<SF_LIVE_REFRESH_TOKEN>',
    login_url     'https://test.salesforce.com');   -- or login.salesforce.com

-- a) describe resolves a real schema
DESCRIBE SELECT * FROM sf.Account;

-- b) scan returns typed rows
SELECT Id, Name FROM sf.Account LIMIT 5;

-- c) server-side predicate pushdown returns a filtered subset
SELECT COUNT(*) FROM sf.Account WHERE Name != '';

-- d) multi-page pagination (only if the org has > 2000 Accounts)
SELECT COUNT(*) FROM sf.Account;

DETACH sf;
```

## 5. Expected results

- ATTACH succeeds (no error) — OAuth refresh-token exchange + `instance_url`
  discovery worked over real HTTPS with TLS verification on.
- (a) `DESCRIBE` lists `Id`, `Name`, etc. with mapped DuckDB types; compound
  `address`/`location` fields are absent (expected, v0.1).
- (b) returns up to 5 rows with correct types.
- (c) returns a count; confirms the SOQL `WHERE` filtered server-side.
- (d) returns the full count, exercising `queryMore` pagination if applicable.

## 6. Evidence checklist

Record in a **secret-free** note (not committed with secrets):

- [ ] Build: platform + `cmake --build` exit 0
- [ ] `salesforce_oauth_live.test`: pass (or document why sandbox used §4)
- [ ] `salesforce_describe_live.test`: pass
- [ ] `salesforce_query_live.test`: pass
- [ ] `salesforce_scan_live.test`: pass
- [ ] §4 (a)–(d) ran and returned the expected shapes
- [ ] No secret/token appears in any captured output (grep the evidence for the
      client secret / token strings → must be absent)
- [ ] Org type recorded (sandbox / scratch / Dev), API version used
- [ ] Date + operator (who ran it)

## 7. Criteria to release the `v0.1.0` tag

All must hold before a human tags `v0.1.0` (in `flozer/duckdb-salesforce` only):

1. Offline suite green (166 assertions; see README → Testing).
2. This smoke test passed against a real sandbox/scratch org with evidence
   captured (§6), secret-free.
3. Build verified on the intended release platform(s).
4. No outstanding correctness/security concern.
5. Explicit human approval to tag.

Tagging is a human action; the assistant will only create the tag on explicit
instruction. **No `duckdb/community-extensions` action at this stage (C.5).**

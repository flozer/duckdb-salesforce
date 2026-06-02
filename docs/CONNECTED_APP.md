# Connected App + Refresh Token (for the smoke test)

How to create a Connected App and mint a **refresh token** so you can run the
[SMOKE.md](../SMOKE.md) validation. **Sandbox / scratch / Developer Edition
only.** Never paste any secret into an issue, chat, log, or commit.

## Why not username + password?

The username-password OAuth flow is **not usable** here:

- the extension only implements the **refresh-token** flow — `ATTACH` needs
  `client_id` + `client_secret` + `refresh_token` (no username/password path);
- [SECURITY.md](../SECURITY.md) forbids the username-password flow (refresh
  token / JWT only);
- Salesforce **disables** the username-password OAuth flow by default on modern
  orgs.

You will collect: `client_id`, `client_secret`, `refresh_token`, and the login
host. `domain=login` (production/dev) or `domain=test` (sandbox) maps to
`SF_LIVE_LOGIN_URL` = `https://login.salesforce.com` / `https://test.salesforce.com`.

## 1. Create the Connected App

Setup → **App Manager** → **New Connected App** (or New Connected App in the
classic UI):

- **Connected App Name** / **API Name**: e.g. `duckdb_salesforce_smoke`
- **Contact Email**: yours
- ✅ **Enable OAuth Settings**
- **Callback URL**: `http://localhost:1717/OauthRedirect`
  (any URL you control; only needed for the web-server flow in §3-B)
- **Selected OAuth Scopes**: add
  - `Manage user data via APIs (api)`
  - `Perform requests at any time (refresh_token, offline_access)`
- ✅ **Require Secret for Web Server Flow** (so `client_secret` is used)
- Optional, for the easiest minting in §3-A: ✅ **Enable Device Flow**
- Save. **Wait ~2–10 minutes** for it to propagate.

Then open the app → **Manage Consumer Details** to read:

- **Consumer Key** → `SF_LIVE_CLIENT_ID`
- **Consumer Secret** → `SF_LIVE_CLIENT_SECRET`

## 2. OAuth policies + test user

App → **Manage** → **Edit Policies**:

- **Permitted Users**: "Admin approved users are pre-authorized" (then assign
  your profile/permset) or "All users may self-authorize" for a quick test.
- **Refresh Token Policy**: "Refresh token is valid until revoked" (or a window
  that outlasts your test).
- **IP Relaxation**: relax if your test runs outside trusted IP ranges.

Ensure the **test user** has **API Enabled** and **read access to Account**.

> Pick the login host now: production/dev = `https://login.salesforce.com`,
> sandbox = `https://test.salesforce.com`. Use it in every URL below.

## 3. Mint the refresh token (pick ONE)

The token response contains secrets — **do not** paste it anywhere shared.

### 3-A. Device Flow (easiest; no redirect handling)

Requires "Enable Device Flow" (§1). Replace `<CID>` and the host as needed.

```sh
# 1) request a device code
curl https://login.salesforce.com/services/oauth2/token \
  -d response_type=device_code -d client_id=<CID> -d scope='api refresh_token'
# -> { "device_code": "...", "user_code": "...", "verification_uri": "...", "interval": 5 }

# 2) open verification_uri in a browser, enter user_code, approve

# 3) poll for the tokens (repeat until it stops returning authorization_pending)
curl https://login.salesforce.com/services/oauth2/token \
  -d grant_type=device -d client_id=<CID> -d code=<device_code>
# -> { "access_token": "...", "refresh_token": "...", "instance_url": "...", ... }
```

Copy the `refresh_token` value (secret) — see §4 to store it safely.

### 3-B. Web-Server Flow (browser + one exchange)

```sh
# 1) open this in a browser, approve; you get redirected to
#    <CALLBACK>?code=<AUTH_CODE>  (URL-decode the code if needed)
https://login.salesforce.com/services/oauth2/authorize?response_type=code&client_id=<CID>&redirect_uri=http://localhost:1717/OauthRedirect&scope=api%20refresh_token

# 2) exchange the code for tokens
curl https://login.salesforce.com/services/oauth2/token \
  -d grant_type=authorization_code -d code=<AUTH_CODE> \
  -d client_id=<CID> -d client_secret=<CSECRET> \
  -d redirect_uri=http://localhost:1717/OauthRedirect
# -> { "access_token": "...", "refresh_token": "...", "instance_url": "...", ... }
```

(Sandbox: replace `login.salesforce.com` with `test.salesforce.com`.)

## 4. Store the values as env vars (no echo, no history)

PowerShell 7 — masked, not saved to shell history:

```powershell
$env:SF_LIVE_CLIENT_ID     = Read-Host 'SF_LIVE_CLIENT_ID'
$env:SF_LIVE_CLIENT_SECRET = Read-Host 'SF_LIVE_CLIENT_SECRET' -MaskInput
$env:SF_LIVE_REFRESH_TOKEN = Read-Host 'SF_LIVE_REFRESH_TOKEN' -MaskInput
$env:SF_LIVE_LOGIN_URL     = 'https://login.salesforce.com'   # or https://test.salesforce.com
```

Verify (names only): `Get-ChildItem Env:SF_LIVE_* | Select-Object Name`.

Then follow [SMOKE.md](../SMOKE.md) §3/§4 to run the validation. Clear after:
`Remove-Item Env:SF_LIVE_*`.

## 5. Security

- Never commit `.env`, curl output, terminal transcript, or anything with a
  token. Don't paste `client_secret` / `refresh_token` / `access_token` in
  issues, PRs, chat, or logs.
- If a value leaks, **revoke** it: Setup → Connected Apps OAuth Usage → revoke,
  or reset the Consumer Secret, and re-mint.
- **C.5:** none of this authorizes any action on `duckdb/community-extensions`.

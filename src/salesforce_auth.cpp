// Salesforce OAuth 2.0 token exchange (issue #3 + #v1.0 Auth UX cut 2).
//
// Two grants, one response path:
//  - ExchangeRefreshToken: grant_type=refresh_token (options/env/sfdx_url).
//  - ExchangeJwtBearer: RS256-signed JWT assertion, no refresh token.
// AcquireToken picks the flow from config.auth_method (also used for 401
// re-auth — a JWT is simply re-signed).
//
// Security invariants (SECURITY.md / ARCHITECTURE.md C.4):
//  - request bodies carry secrets (client_secret / refresh_token / the signed
//    assertion) and are NEVER logged;
//  - the private key, the assembled JWT, and the assertion NEVER appear in any
//    error message; thrown errors carry only HTTP status + Salesforce error
//    codes;
//  - nothing is written to disk.

#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"

#include "duckdb/common/exception.hpp"

#include <cctype>
#include <cstdint>
#include <ctime>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace duckdb {

// application/x-www-form-urlencoded percent-encoding (RFC 3986 unreserved set
// kept verbatim, everything else %XX).
static string FormEncode(const string &s) {
    static const char *hex = "0123456789ABCDEF";
    string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

static string TrimTrailingSlash(const string &s) {
    string out = s;
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// Shared non-200 / missing-field handling. Surfaces only Salesforce's error
// code/description + an actionable, secret-free hint — never the request body.
static SalesforceTokenSet HandleTokenResponse(const HttpResponse &resp, const string &url) {
    if (!resp.transport_ok) {
        throw IOException("salesforce OAuth: token request failed to reach %s (%s).",
                          url, resp.transport_error);
    }
    if (resp.status != 200) {
        string err = sfjson::GetString(resp.body, "error");
        string desc = sfjson::GetString(resp.body, "error_description");
        if (err.empty()) {
            err = "unknown_error";
        }
        string hint;
        if (err == "invalid_grant") {
            hint = " (refresh token / JWT assertion is invalid, expired, or the "
                   "user is not authorized for this Connected App — re-authorize)";
        } else if (err == "invalid_client" || err == "invalid_client_id") {
            hint = " (client_id / client_secret is incorrect for this org)";
        }
        throw IOException(
            "salesforce OAuth token exchange failed (HTTP %d): %s%s%s%s.",
            resp.status, err, desc.empty() ? "" : " - ", desc, hint);
    }

    SalesforceTokenSet ts;
    ts.access_token = sfjson::GetString(resp.body, "access_token");
    ts.instance_url = sfjson::GetString(resp.body, "instance_url");
    if (ts.access_token.empty()) {
        throw IOException(
            "salesforce OAuth: token response did not contain an access_token.");
    }
    if (ts.instance_url.empty()) {
        throw IOException(
            "salesforce OAuth: token response did not contain an instance_url.");
    }
    return ts;
}

SalesforceTokenSet SalesforceAuth::ExchangeRefreshToken(const SalesforceConfig &config,
                                                        SalesforceHttpClient &client) {
    HttpRequest req;
    req.url = TrimTrailingSlash(config.login_url) + "/services/oauth2/token";
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
                   {"Accept", "application/json"}};
    // Body carries the secrets — never logged anywhere.
    req.body = "grant_type=refresh_token" + string("&client_id=") +
               FormEncode(config.client_id) + "&client_secret=" +
               FormEncode(config.client_secret) + "&refresh_token=" +
               FormEncode(config.refresh_token);

    HttpResponse resp = client.Post(req);
    return HandleTokenResponse(resp, req.url);
}

// --- JWT bearer ------------------------------------------------------------

// base64url (RFC 7515): standard base64, '+'->'-', '/'->'_', no '=' padding.
static string Base64Url(const unsigned char *data, size_t len) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
    }
    if (i < len) { // 1 or 2 trailing bytes, no padding
        uint32_t n = data[i] << 16;
        bool two = (i + 1 < len);
        if (two) {
            n |= data[i + 1] << 8;
        }
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        if (two) {
            out.push_back(tbl[(n >> 6) & 63]);
        }
    }
    return out;
}

static string Base64Url(const string &s) {
    return Base64Url(reinterpret_cast<const unsigned char *>(s.data()), s.size());
}

// Minimal JSON string escaping for the claim values (client_id / username /
// login_url). Escapes the two characters that would break a JSON string.
static string JsonEscape(const string &s) {
    string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

// RS256-sign `input` with the PEM private key. Throws a secret-free error on
// any OpenSSL failure (never echoes the key or the input).
static string SignRs256(const string &pem, const string &input) {
    BIO *bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) {
        throw IOException("salesforce OAuth (jwt): out of memory loading private key.");
    }
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        throw IOException(
            "salesforce OAuth (jwt): could not parse the private key (expected an "
            "unencrypted PKCS#1/PKCS#8 PEM RSA key).");
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    string sig;
    bool ok = ctx != nullptr;
    if (ok) {
        ok = EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1;
    }
    size_t siglen = 0;
    if (ok) {
        ok = EVP_DigestSign(ctx, nullptr, &siglen,
                            reinterpret_cast<const unsigned char *>(input.data()),
                            input.size()) == 1;
    }
    if (ok) {
        sig.resize(siglen);
        ok = EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(&sig[0]), &siglen,
                            reinterpret_cast<const unsigned char *>(input.data()),
                            input.size()) == 1;
    }
    if (ctx) {
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    if (!ok) {
        throw IOException(
            "salesforce OAuth (jwt): failed to RS256-sign the assertion (check "
            "that the private key is a valid unencrypted RSA key).");
    }
    sig.resize(siglen);
    return sig;
}

SalesforceTokenSet SalesforceAuth::ExchangeJwtBearer(const SalesforceConfig &config,
                                                     SalesforceHttpClient &client) {
    // Short-lived assertion: exp = now + 300s (Salesforce caps the window).
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t exp = now + 300;

    string header = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    string claims = "{\"iss\":\"" + JsonEscape(config.client_id) +
                    "\",\"sub\":\"" + JsonEscape(config.username) +
                    "\",\"aud\":\"" + JsonEscape(TrimTrailingSlash(config.login_url)) +
                    "\",\"iat\":" + std::to_string(now) +
                    ",\"exp\":" + std::to_string(exp) + "}";

    string signing_input = Base64Url(header) + "." + Base64Url(claims);
    string sig = SignRs256(config.private_key, signing_input);
    string assertion = signing_input + "." + Base64Url(sig);

    HttpRequest req;
    req.url = TrimTrailingSlash(config.login_url) + "/services/oauth2/token";
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"},
                   {"Accept", "application/json"}};
    // Body carries the signed assertion — never logged anywhere.
    req.body = "grant_type=" +
               FormEncode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
               "&assertion=" + FormEncode(assertion);

    HttpResponse resp = client.Post(req);
    return HandleTokenResponse(resp, req.url);
}

SalesforceTokenSet SalesforceAuth::AcquireToken(const SalesforceConfig &config,
                                                SalesforceHttpClient &client) {
    if (config.auth_method == SalesforceAuthMethod::kJwt) {
        return ExchangeJwtBearer(config, client);
    }
    return ExchangeRefreshToken(config, client);
}

} // namespace duckdb

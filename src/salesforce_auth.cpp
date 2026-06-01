// Salesforce OAuth 2.0 refresh-token exchange (issue #3).
//
// Pure logic over the SalesforceHttpClient interface, so it is fully
// unit-testable with a mock transport — CI never contacts Salesforce.
//
// Security invariants (SECURITY.md / ARCHITECTURE.md C.4):
//  - the request body carries client_secret + refresh_token and is NEVER
//    logged;
//  - thrown errors contain only HTTP status + Salesforce error codes, never a
//    secret or token value;
//  - nothing is written to disk.

#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"

#include "duckdb/common/exception.hpp"

#include <cctype>

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

// Extract a top-level JSON string value by key. Sufficient for Salesforce
// token-endpoint responses (flat objects of string values). Handles standard
// backslash escapes inside the value. Returns "" if the key is absent.
// NOTE: intentionally tiny — a full JSON parser can replace this later without
// changing the public contract.
static string ExtractJsonString(const string &json, const string &key) {
    const string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == string::npos) {
        return "";
    }
    size_t i = k + needle.size();
    // skip whitespace + ':'
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' ||
                               json[i] == '\r')) {
        i++;
    }
    if (i >= json.size() || json[i] != ':') {
        return "";
    }
    i++;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' ||
                               json[i] == '\r')) {
        i++;
    }
    if (i >= json.size() || json[i] != '"') {
        return ""; // not a string value
    }
    i++; // opening quote
    string out;
    while (i < json.size()) {
        char c = json[i++];
        if (c == '\\' && i < json.size()) {
            char e = json[i++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '/': out.push_back('/'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: out.push_back(e); break; // includes \uXXXX left raw
            }
        } else if (c == '"') {
            return out; // closing quote
        } else {
            out.push_back(c);
        }
    }
    return ""; // unterminated string
}

static string TrimTrailingSlash(const string &s) {
    string out = s;
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
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

    if (!resp.transport_ok) {
        // transport_error is produced by the transport and is secret-free.
        throw IOException("salesforce OAuth: token request failed to reach %s (%s).",
                          req.url, resp.transport_error);
    }

    if (resp.status != 200) {
        // Surface only Salesforce's error code/description — never the body
        // wholesale and never the request secrets.
        string err = ExtractJsonString(resp.body, "error");
        string desc = ExtractJsonString(resp.body, "error_description");
        if (err.empty()) {
            err = "unknown_error";
        }
        throw IOException(
            "salesforce OAuth token exchange failed (HTTP %d): %s%s%s.",
            resp.status, err, desc.empty() ? "" : " - ", desc);
    }

    SalesforceTokenSet ts;
    ts.access_token = ExtractJsonString(resp.body, "access_token");
    ts.instance_url = ExtractJsonString(resp.body, "instance_url");

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

} // namespace duckdb

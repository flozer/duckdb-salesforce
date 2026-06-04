// Salesforce ATTACH configuration parsing + validation (issue #2).
//
// Pure, offline: turns the ATTACH path + option map into a validated
// SalesforceConfig, or throws a clear BinderException. No network, no token
// persistence, and no secret value ever appears in an error message.

#include "salesforce_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace duckdb {

static constexpr const char *kScheme = "salesforce://";

// Resolve an environment variable for auth (#v1.0 Auth UX). TEST-ONLY: when the
// `sf_mock_env` setting is set ("NAME=value;NAME2=value2"), it overrides the OS
// environment so the offline suite can exercise env/sfdx_url auth without
// touching the real environment. Returns false (out untouched) if absent.
// The value is a secret — never logged.
static bool EnvLookup(ClientContext &context, const char *name, string &out) {
    Value mv;
    if (context.TryGetCurrentSetting("sf_mock_env", mv) && !mv.IsNull()) {
        string blob = mv.ToString();
        string probe = blob;
        StringUtil::LTrim(probe);
        StringUtil::RTrim(probe);
        // Only treat sf_mock_env as an override when it is non-empty. The
        // setting defaults to "" (non-null), so an empty value MUST fall
        // through to the real OS environment — otherwise env/sfdx_url/jwt auth
        // would be dead outside the offline tests that set this hook.
        if (!probe.empty()) {
            for (auto &entry : StringUtil::Split(blob, ';')) {
                auto eq = entry.find('=');
                if (eq == string::npos) {
                    continue;
                }
                string k = entry.substr(0, eq);
                StringUtil::LTrim(k);
                StringUtil::RTrim(k);
                if (k == name) {
                    out = entry.substr(eq + 1);
                    return !out.empty();
                }
            }
            return false; // mock active but this var absent
        }
        // empty sf_mock_env -> fall through to the real OS environment
    }
    const char *v = std::getenv(name);
    if (v == nullptr || v[0] == '\0') {
        return false;
    }
    out = v;
    return true;
}

// Required env var: clear, secret-free error naming ONLY the variable.
static string RequireEnv(ClientContext &context, const char *name) {
    string v;
    if (!EnvLookup(context, name, v)) {
        throw BinderException(
            "salesforce ATTACH: missing required environment variable '%s'.", name);
    }
    return v;
}

// StringUtil::Trim mutates in place and returns void; this returns a trimmed
// copy so it can be used in expressions.
static string Trimmed(string s) {
    StringUtil::LTrim(s);
    StringUtil::RTrim(s);
    return s;
}

// Required-option helper: present AND non-empty (after trim). Never echoes
// the value — only the key — so this is safe for the secret options.
static string RequireOption(const case_insensitive_map_t<string> &opts,
                            const char *key) {
    auto it = opts.find(key);
    if (it == opts.end() || Trimmed(it->second).empty()) {
        throw BinderException(
            "salesforce ATTACH: missing required option '%s'. Expected: "
            "ATTACH 'salesforce://<org>' (client_id '...', client_secret '...', "
            "refresh_token '...').",
            key);
    }
    return it->second;
}

// login_url must be an absolute https:// URL with a host. The value is not a
// secret, so it is safe to include in the error message.
static void ValidateLoginUrl(const string &url) {
    static const string kHttps = "https://";
    if (!StringUtil::StartsWith(StringUtil::Lower(url), kHttps) ||
        url.size() <= kHttps.size()) {
        throw BinderException(
            "salesforce ATTACH: 'login_url' must be an absolute https:// URL "
            "(got: '%s').",
            url);
    }
}

// api_version accepts "vNN.N" or "NN.N" (case-insensitive 'v'); returns the
// normalised "vNN.N" form. Not a secret — safe to echo on error.
static string NormaliseApiVersion(const string &raw) {
    string v = Trimmed(raw);
    size_t i = 0;
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) {
        i = 1;
    }
    bool seen_digit = false, seen_dot = false, digit_after_dot = false;
    for (size_t p = i; p < v.size(); p++) {
        char c = v[p];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            if (seen_dot) {
                digit_after_dot = true;
            } else {
                seen_digit = true;
            }
        } else if (c == '.' && !seen_dot && seen_digit) {
            seen_dot = true;
        } else {
            seen_digit = false; // force the failure below
            break;
        }
    }
    if (!seen_digit || !seen_dot || !digit_after_dot) {
        throw BinderException(
            "salesforce ATTACH: 'api_version' must look like 'vNN.N' or 'NN.N' "
            "(got: '%s').",
            raw);
    }
    return "v" + v.substr(i);
}

// Parse a SFDX auth URL "force://<clientId>:<clientSecret>:<refreshToken>@<host>"
// into the config. NEVER echoes the URL or any component on error.
static void ParseSfdxAuthUrl(const string &url, SalesforceConfig &cfg) {
    static const string kForce = "force://";
    auto fail = []() {
        throw BinderException(
            "salesforce ATTACH: SF_SFDX_AUTH_URL is not a valid SFDX auth URL "
            "(expected force://<clientId>:<clientSecret>:<refreshToken>@<instance>).");
    };
    if (!StringUtil::StartsWith(url, kForce)) {
        fail();
    }
    string rest = url.substr(kForce.size());
    auto at = rest.rfind('@'); // host has no '@'; tokens don't either
    if (at == string::npos) {
        fail();
    }
    string creds = rest.substr(0, at);
    string host = Trimmed(rest.substr(at + 1));
    while (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    if (host.empty()) {
        fail();
    }
    // creds = clientId : clientSecret : refreshToken (clientSecret may be empty)
    auto p1 = creds.find(':');
    auto p2 = (p1 == string::npos) ? string::npos : creds.find(':', p1 + 1);
    if (p1 == string::npos || p2 == string::npos) {
        fail();
    }
    cfg.client_id = creds.substr(0, p1);
    cfg.client_secret = creds.substr(p1 + 1, p2 - p1 - 1);
    cfg.refresh_token = creds.substr(p2 + 1);
    if (cfg.client_id.empty() || cfg.refresh_token.empty()) {
        fail();
    }
    cfg.login_url = "https://" + host;
    ValidateLoginUrl(cfg.login_url);
}

// Read a PEM private-key file into memory. The PATH is not a secret (safe to
// echo on error); the CONTENTS are SENSITIVE and never logged. Only unencrypted
// PKCS#1/PKCS#8 PEM is supported — an encrypted key is rejected with a clear,
// secret-free message.
static string LoadPrivateKeyFile(const string &pathv) {
    std::ifstream f(pathv, std::ios::binary);
    if (!f) {
        throw BinderException(
            "salesforce ATTACH (jwt): cannot open private key file '%s'.", pathv);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    string pem = ss.str();
    if (pem.find("-----BEGIN") == string::npos) {
        throw BinderException(
            "salesforce ATTACH (jwt): file '%s' is not a PEM private key.", pathv);
    }
    if (pem.find("ENCRYPTED") != string::npos) {
        throw BinderException(
            "salesforce ATTACH (jwt): encrypted private keys are not supported; "
            "provide an unencrypted PKCS#1/PKCS#8 PEM key (file '%s').",
            pathv);
    }
    return pem;
}

SalesforceConfig SalesforceConfig::ParseAndValidate(const string &path, AttachInfo &info,
                                                    ClientContext &context) {
    SalesforceConfig cfg;

    // --- path: salesforce://<org> ------------------------------------------
    if (!StringUtil::StartsWith(StringUtil::Lower(path), kScheme)) {
        throw BinderException(
            "salesforce ATTACH: path must start with 'salesforce://' "
            "(got: '%s').",
            path);
    }
    cfg.org = Trimmed(path.substr(string(kScheme).size()));
    if (cfg.org.empty()) {
        throw BinderException(
            "salesforce ATTACH: missing org in path. Expected "
            "'salesforce://<org>' (e.g. 'salesforce://production').");
    }

    // --- options: lower-case keys, reject unknowns (helps catch typos) -----
    case_insensitive_map_t<string> opts;
    for (auto &kv : info.options) {
        const string &key = kv.first;
        if (StringUtil::CIEquals(key, "type")) {
            continue; // DuckDB routing key
        }
        if (!StringUtil::CIEquals(key, "auth_source") &&
            !StringUtil::CIEquals(key, "client_id") &&
            !StringUtil::CIEquals(key, "client_secret") &&
            !StringUtil::CIEquals(key, "refresh_token") &&
            !StringUtil::CIEquals(key, "username") &&
            !StringUtil::CIEquals(key, "private_key_file") &&
            !StringUtil::CIEquals(key, "login_url") &&
            !StringUtil::CIEquals(key, "api_version")) {
            throw BinderException(
                "salesforce ATTACH: unknown option '%s'. Valid options: "
                "auth_source, client_id, client_secret, refresh_token, "
                "username, private_key_file, login_url, api_version.",
                key);
        }
        opts[key] = kv.second.ToString();
    }

    // --- auth_source: where the credentials come from (#v1.0 Auth UX) ------
    string auth_source = "options";
    auto as_it = opts.find("auth_source");
    if (as_it != opts.end() && !Trimmed(as_it->second).empty()) {
        auth_source = StringUtil::Lower(Trimmed(as_it->second));
    }

    if (auth_source == "options") {
        cfg.client_id = RequireOption(opts, "client_id");
        cfg.client_secret = RequireOption(opts, "client_secret");
        cfg.refresh_token = RequireOption(opts, "refresh_token");
        auto login_it = opts.find("login_url");
        if (login_it != opts.end() && !Trimmed(login_it->second).empty()) {
            cfg.login_url = Trimmed(login_it->second);
            ValidateLoginUrl(cfg.login_url);
        } else {
            cfg.login_url = kDefaultLoginUrl;
        }
    } else if (auth_source == "env") {
        // SF_CLIENT_ID / SF_CLIENT_SECRET / SF_REFRESH_TOKEN [+ SF_LOGIN_URL].
        cfg.client_id = RequireEnv(context, "SF_CLIENT_ID");
        cfg.client_secret = RequireEnv(context, "SF_CLIENT_SECRET");
        cfg.refresh_token = RequireEnv(context, "SF_REFRESH_TOKEN");
        string login;
        if (EnvLookup(context, "SF_LOGIN_URL", login)) {
            cfg.login_url = Trimmed(login);
            ValidateLoginUrl(cfg.login_url);
        } else {
            cfg.login_url = kDefaultLoginUrl;
        }
    } else if (auth_source == "sfdx_url") {
        // SFDX auth URL from SF_SFDX_AUTH_URL.
        ParseSfdxAuthUrl(RequireEnv(context, "SF_SFDX_AUTH_URL"), cfg);
    } else if (auth_source == "jwt") {
        // JWT bearer flow (#v1.0 Auth UX cut 2): no client_secret / refresh
        // token. iss=client_id, sub=username, signed with a local PEM key.
        cfg.auth_method = SalesforceAuthMethod::kJwt;
        cfg.client_id = RequireOption(opts, "client_id");
        cfg.username = RequireOption(opts, "username");
        // Key PATH: inline private_key_file option, else SF_JWT_KEY_FILE env.
        // Docs recommend the env var for pipelines; inline is for local dev.
        string key_path;
        auto pk_it = opts.find("private_key_file");
        if (pk_it != opts.end() && !Trimmed(pk_it->second).empty()) {
            key_path = Trimmed(pk_it->second);
        } else if (!EnvLookup(context, "SF_JWT_KEY_FILE", key_path)) {
            throw BinderException(
                "salesforce ATTACH (jwt): set 'private_key_file' or the "
                "SF_JWT_KEY_FILE environment variable to a PEM private key path.");
        }
        cfg.private_key = LoadPrivateKeyFile(Trimmed(key_path));
        auto login_it = opts.find("login_url");
        if (login_it != opts.end() && !Trimmed(login_it->second).empty()) {
            cfg.login_url = Trimmed(login_it->second);
            ValidateLoginUrl(cfg.login_url);
        } else {
            cfg.login_url = kDefaultLoginUrl;
        }
    } else {
        throw BinderException(
            "salesforce ATTACH: 'auth_source' must be 'options', 'env', "
            "'sfdx_url', or 'jwt' (got: '%s').",
            auth_source);
    }

    // --- api_version: from option or default, any auth_source --------------
    auto ver_it = opts.find("api_version");
    if (ver_it != opts.end() && !Trimmed(ver_it->second).empty()) {
        cfg.api_version = NormaliseApiVersion(ver_it->second);
    } else {
        cfg.api_version = kDefaultApiVersion;
    }

    return cfg;
}

} // namespace duckdb

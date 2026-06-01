// Salesforce ATTACH configuration parsing + validation (issue #2).
//
// Pure, offline: turns the ATTACH path + option map into a validated
// SalesforceConfig, or throws a clear BinderException. No network, no token
// persistence, and no secret value ever appears in an error message.

#include "salesforce_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

#include <cctype>

namespace duckdb {

static constexpr const char *kScheme = "salesforce://";

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

SalesforceConfig SalesforceConfig::ParseAndValidate(const string &path,
                                                    AttachInfo &info) {
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
        // DuckDB may surface the routing key "type" here; ignore it.
        if (StringUtil::CIEquals(key, "type")) {
            continue;
        }
        if (!StringUtil::CIEquals(key, "client_id") &&
            !StringUtil::CIEquals(key, "client_secret") &&
            !StringUtil::CIEquals(key, "refresh_token") &&
            !StringUtil::CIEquals(key, "login_url") &&
            !StringUtil::CIEquals(key, "api_version")) {
            throw BinderException(
                "salesforce ATTACH: unknown option '%s'. Valid options: "
                "client_id, client_secret, refresh_token, login_url, "
                "api_version.",
                key);
        }
        opts[key] = kv.second.ToString();
    }

    // --- required ----------------------------------------------------------
    cfg.client_id = RequireOption(opts, "client_id");
    cfg.client_secret = RequireOption(opts, "client_secret");
    cfg.refresh_token = RequireOption(opts, "refresh_token");

    // --- optional with defaults --------------------------------------------
    auto login_it = opts.find("login_url");
    if (login_it != opts.end() && !Trimmed(login_it->second).empty()) {
        cfg.login_url = Trimmed(login_it->second);
        ValidateLoginUrl(cfg.login_url);
    } else {
        cfg.login_url = kDefaultLoginUrl;
    }

    auto ver_it = opts.find("api_version");
    if (ver_it != opts.end() && !Trimmed(ver_it->second).empty()) {
        cfg.api_version = NormaliseApiVersion(ver_it->second);
    } else {
        cfg.api_version = kDefaultApiVersion;
    }

    return cfg;
}

} // namespace duckdb

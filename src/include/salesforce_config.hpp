#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct AttachInfo;

// Parsed, validated connection configuration for a single ATTACH.
//
// v0.1 (issue #2): this is the in-memory result of parsing
//
//     ATTACH 'salesforce://<org>'
//         (client_id '...', client_secret '...', refresh_token '...'
//          [, login_url '...'] [, api_version '...']);
//
// No network call is made and nothing is persisted. The two secret fields
// (client_secret, refresh_token) are NEVER written to logs or error messages
// — only their presence/absence is reported.
struct SalesforceConfig {
    // Org alias taken from the path after the salesforce:// scheme
    // (e.g. "production", "sandbox", a custom label). Non-empty.
    string org;

    // Required OAuth Connected App credentials.
    string client_id;
    string client_secret; // SENSITIVE — never log/echo
    string refresh_token; // SENSITIVE — never log/echo

    // Optional. Defaults applied during parse.
    string login_url;   // default https://login.salesforce.com
    string api_version; // normalised to "vNN.N", default kDefaultApiVersion

    // Default Salesforce login host and API version used when the
    // corresponding options are omitted.
    static constexpr const char *kDefaultLoginUrl = "https://login.salesforce.com";
    static constexpr const char *kDefaultApiVersion = "v60.0";

    // Parse + validate. Throws BinderException with a clear, secret-free
    // message on any invalid/missing field. No network, no persistence.
    static SalesforceConfig ParseAndValidate(const string &path, AttachInfo &info);
};

} // namespace duckdb

#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct AttachInfo;
class ClientContext;

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
// Which OAuth grant the token exchange uses. Selected by auth_source during
// parse; drives SalesforceAuth::AcquireToken.
enum class SalesforceAuthMethod {
    kRefreshToken, // refresh-token flow (options / env / sfdx_url sources)
    kJwt           // JWT bearer flow (auth_source = 'jwt')
};

struct SalesforceConfig {
    // Org alias taken from the path after the salesforce:// scheme
    // (e.g. "production", "sandbox", a custom label). Non-empty.
    string org;

    // How the token is obtained. Default = refresh-token.
    SalesforceAuthMethod auth_method = SalesforceAuthMethod::kRefreshToken;

    // Connected App client id (issuer for JWT). Required by all flows.
    string client_id;
    string client_secret; // SENSITIVE — never log/echo (refresh-token flows)
    string refresh_token; // SENSITIVE — never log/echo (refresh-token flows)

    // JWT bearer flow (#v1.0 Auth UX cut 2). `username` is the JWT subject;
    // `private_key` holds the PEM contents loaded at parse (SENSITIVE — never
    // log/echo; never persisted). Empty for refresh-token flows.
    string username;
    string private_key; // SENSITIVE — never log/echo

    // Optional. Defaults applied during parse.
    string login_url;   // default https://login.salesforce.com
    string api_version; // normalised to "vNN.N", default kDefaultApiVersion

    // Default Salesforce login host and API version used when the
    // corresponding options are omitted.
    static constexpr const char *kDefaultLoginUrl = "https://login.salesforce.com";
    static constexpr const char *kDefaultApiVersion = "v60.0";

    // Parse + validate. Throws BinderException with a clear, secret-free
    // message on any invalid/missing field. No network, no persistence.
    // `context` is used to resolve env/sfdx_url auth sources (#v1.0 Auth UX).
    static SalesforceConfig ParseAndValidate(const string &path, AttachInfo &info,
                                             ClientContext &context);
};

} // namespace duckdb

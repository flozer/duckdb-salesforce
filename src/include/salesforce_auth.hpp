#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct SalesforceConfig;
class SalesforceHttpClient;

// In-memory result of a successful OAuth 2.0 token exchange. access_token is
// SENSITIVE and must never be logged or echoed; instance_url is not a secret.
struct SalesforceTokenSet {
    string access_token; // SENSITIVE — never log/echo
    string instance_url; // e.g. https://myorg.my.salesforce.com (not secret)
};

class SalesforceAuth {
public:
    // OAuth 2.0 refresh-token flow (#3): POST to
    //   <login_url>/services/oauth2/token
    // with grant_type=refresh_token + client_id/client_secret/refresh_token.
    // Returns the access_token + instance_url, held in memory by the caller.
    //
    // Guarantees:
    //  - the request body (which carries the secrets) is never logged;
    //  - no secret or token ever appears in a thrown error message — only
    //    Salesforce error codes/descriptions and HTTP status are surfaced;
    //  - nothing is persisted to disk.
    //
    // Throws on transport failure, non-200 status, or a response missing
    // access_token / instance_url.
    static SalesforceTokenSet ExchangeRefreshToken(const SalesforceConfig &config,
                                                   SalesforceHttpClient &client);
};

} // namespace duckdb

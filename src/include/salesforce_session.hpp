#pragma once

#include "duckdb.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"

namespace duckdb {

struct SalesforceDescribe;
class SalesforceHttpClient;

// Paginated SOQL query result: raw JSON object per record, in fetch order.
// Row -> DuckDB vector decoding is issue #7; #6 only fetches.
struct SalesforceQueryResult {
    vector<string> records;
    idx_t page_count = 0;
};

// A live (mock-injectable) session against one Salesforce org: holds the
// OAuth token and performs authenticated GETs with a single
// 401 -> refresh -> retry. Token is in memory only and never logged.
class SalesforceSession {
public:
    SalesforceSession(SalesforceConfig config, SalesforceHttpClient &client);

    // Initial refresh-token exchange. Must be called before any request.
    void Authenticate();

    // GET /services/data/<api_version>/sobjects/<object>/describe -> schema.
    SalesforceDescribe Describe(const string &object);

    // Run a SOQL query: GET /services/data/<api_version>/query?q=<encoded>, then
    // follow nextRecordsUrl (opaque, used verbatim) until done. Records are
    // collected in fetch order. Guards against pagination loops.
    SalesforceQueryResult Query(const string &soql);

private:
    // Authenticated GET returning the 200 body, or throwing a clear,
    // secret-free error. On HTTP 401 it re-exchanges the refresh token once
    // and retries the request a single time.
    string AuthorizedGet(const string &path);

    SalesforceConfig config_;
    SalesforceHttpClient &client_;
    SalesforceTokenSet token_;
};

} // namespace duckdb

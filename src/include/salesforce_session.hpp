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

// One page of a SOQL query (used by the lazy/streaming scan, #11).
struct SalesforceQueryPage {
    vector<string> records;
    string next_path; // nextRecordsUrl ("" when there are no more pages)
    bool done = true;
};

// A live (mock-injectable) session against one Salesforce org: holds the
// OAuth token and performs authenticated GETs with a single
// 401 -> refresh -> retry. Token is in memory only and never logged.
class SalesforceSession {
public:
    SalesforceSession(SalesforceConfig config, SalesforceHttpClient &client);

    // Initial refresh-token exchange. Must be called before any request.
    void Authenticate();

    // Reuse a token obtained earlier (e.g. at ATTACH) instead of re-exchanging.
    void SetToken(SalesforceTokenSet token) {
        token_ = std::move(token);
    }
    const SalesforceTokenSet &Token() const {
        return token_;
    }

    // GET /services/data/<api_version>/sobjects/<object>/describe -> schema.
    SalesforceDescribe Describe(const string &object);

    // GET /services/data/<api_version>/sobjects -> names of queryable sObjects
    // (global describe; names + flags only, no fields). For object listing (#14).
    vector<string> GlobalDescribe();

    // Run a SOQL query eagerly: fetch every page (used by salesforce_query #6).
    // Implemented over FetchPage. Guards against pagination loops.
    SalesforceQueryResult Query(const string &soql);

    // Initial query path for a SOQL string:
    // /services/data/<api_version>/query?q=<url-encoded>.
    string QueryPath(const string &soql) const;

    // Fetch ONE page — the initial query path or an opaque nextRecordsUrl —
    // authenticated GET with 401 -> refresh -> retry. The caller owns
    // pagination state + loop guards (the lazy scan in #11 uses this).
    SalesforceQueryPage FetchPage(const string &path);

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

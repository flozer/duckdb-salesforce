#pragma once

#include "duckdb.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"

#include <unordered_map>

namespace duckdb {

struct SalesforceDescribe;
struct HttpResponse;
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

// One Bulk API 2.0 result CSV page (#v0.7 §8 — lazy streaming). `columns` is the
// page's header; `rows` the data rows (header stripped); `next_locator` is the
// Sforce-Locator for the next page ("" when this is the last page).
struct SalesforceBulkPage {
    vector<string> columns;
    vector<vector<string>> rows;
    string next_locator;
};

// Snapshot of the org's REST /limits relevant to the quota governor (#v0.4).
// `available` is false when /limits could not be read — the governor then
// applies its fail-open/closed policy. -1 means a limit was absent from the
// payload (org variance) and is not checked.
struct SalesforceQuotaSnapshot {
    bool available = false;
    int64_t api_max = -1;       // DailyApiRequests.Max
    int64_t api_remaining = -1; // DailyApiRequests.Remaining
    int64_t bulk_max = -1;      // DailyBulkV2QueryJobs.Max (optional)
    int64_t bulk_remaining = -1;
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

    // queryAll mode (#v0.9 §1): when true, REST uses the /queryAll endpoint and
    // Bulk jobs use operation "queryAll" — both also return archived + soft-
    // deleted (IsDeleted=true) records. Applies to the data query AND the
    // COUNT()/MIN-MAX probes built on QueryPath, so counts/ranges match.
    void SetQueryAll(bool query_all) {
        query_all_ = query_all;
    }
    // Bulk job poll budget (ROADMAP §15): max status polls before BulkStartJob
    // fails fast with a clear poll-count error. Default 600 preserves prior
    // behaviour; a large backfill that genuinely needs more can raise it via
    // sf_bulk_poll_budget. The setting is clamped to int range before it reaches
    // here; BulkStartJob additionally treats any value < 1 as 1.
    void SetBulkPollBudget(int budget) {
        bulk_poll_budget_ = budget;
    }
    const SalesforceTokenSet &Token() const {
        return token_;
    }

    // GET /services/data/<api_version>/sobjects/<object>/describe -> schema.
    SalesforceDescribe Describe(const string &object);

    // Raw describe JSON body for <object> (#v1.3 §14). Used by the picklist /
    // record-type metadata functions, which parse fields the scan schema drops.
    string DescribeJson(const string &object);

    // Tooling API fast schema discovery (#v0.6 §6). One (chunked, paginated)
    // Tooling SOQL over FieldDefinition fetches the fields of MANY objects at
    // once, mapping the coarse DataType display strings. Compound fields are
    // dropped; fields with an ambiguous/unmapped DataType get unknown_type=true
    // so the caller falls back to the authoritative REST Describe per object.
    // Returns false on a transport/HTTP failure (caller falls back fully).
    // Result is keyed by lower-cased object name. Increments the tooling-call
    // counter per Tooling query issued.
    bool ToolingDescribe(const vector<string> &objects,
                         std::unordered_map<string, SalesforceDescribe> &out);

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

    // Bulk API 2.0, lazy result streaming (#v0.7 §8). BulkStartJob creates the
    // query job and polls to JobComplete (Failed/Aborted -> clean error), then
    // returns the base /results path — NO result pages are downloaded. The scan
    // then calls BulkFetchResultPage ON DEMAND, following the Sforce-Locator,
    // so memory no longer scales with the full result. Same auth/401-refresh.
    string BulkStartJob(const string &soql);
    SalesforceBulkPage BulkFetchResultPage(const string &path);

    // PK chunking probe (#v0.7 §9): one REST aggregate `SELECT MIN(Id), MAX(Id)`
    // (optionally with the pushed WHERE) to bound the Id range for splitting.
    // Returns false on any failure or when the object is empty (null min/max),
    // so the caller falls back to a single chunk. Never throws.
    bool TryMinMaxId(const string &object, const string &where, string &min_id, string &max_id);

    // Auto-transport probe (#v0.3 §2): estimate the row count for a COUNT() SOQL
    // via one REST /query call (reads `totalSize`; zero row egress). Returns
    // false on ANY failure (HTTP error, missing totalSize) so the caller falls
    // back to REST. Never throws.
    bool TryEstimateCount(const string &count_soql, int64_t &out_rows);

    // Read the org's REST /limits (#v0.4 quota governor). One API call; the
    // caller caches it. Returns an unavailable snapshot (never throws) on any
    // failure so the governor can apply its fail-open policy.
    SalesforceQuotaSnapshot QueryLimits();

private:
    // Bearer-authenticated request (GET, or POST with a JSON body) that retries
    // ONCE on HTTP 401 after re-exchanging the refresh token. Returns the full
    // response (status + body + headers); the caller decides what is an error.
    // Authorization is never logged.
    HttpResponse AuthorizedSend(bool post, const string &path, const string &json_body);

    // Authenticated GET returning the 200 body, or throwing a clear,
    // secret-free error (used by describe/query).
    string AuthorizedGet(const string &path);

    SalesforceConfig config_;
    SalesforceHttpClient &client_;
    SalesforceTokenSet token_;
    bool query_all_ = false; // #v0.9 §1: /query vs /queryAll + Bulk operation
    int bulk_poll_budget_ = 600; // ROADMAP §15: max Bulk status polls
};

} // namespace duckdb

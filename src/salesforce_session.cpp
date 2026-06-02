// Authenticated Salesforce session (issue #5).
//
// Holds the OAuth token in memory and performs authenticated GETs. On a 401 it
// re-exchanges the refresh token exactly once and retries the request once
// (GET 401 -> re-auth -> GET). Bearer token is never logged; errors carry only
// the request path, HTTP status, and the Salesforce errorCode/message.

#include "salesforce_session.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"

#include "salesforce_csv.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_url.hpp"

#include "duckdb/common/exception.hpp"

#include <chrono>
#include <thread>
#include <unordered_set>

namespace duckdb {

static string TrimTrailingSlash(const string &s) {
    string out = s;
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

SalesforceSession::SalesforceSession(SalesforceConfig config, SalesforceHttpClient &client)
    : config_(std::move(config)), client_(client) {
}

void SalesforceSession::Authenticate() {
    token_ = SalesforceAuth::ExchangeRefreshToken(config_, client_);
}

HttpResponse SalesforceSession::AuthorizedSend(bool post, const string &path,
                                               const string &json_body) {
    auto do_req = [&]() {
        HttpRequest req;
        req.url = TrimTrailingSlash(token_.instance_url) + path;
        req.headers = {{"Authorization", "Bearer " + token_.access_token},
                       {"Accept", "application/json"}};
        if (post) {
            req.headers.push_back({"Content-Type", "application/json"});
            req.body = json_body;
            return client_.Post(req);
        }
        return client_.Get(req);
    };

    HttpResponse resp = do_req();
    if (!resp.transport_ok) {
        throw IOException("salesforce: request to %s failed to reach the server (%s).", path,
                          resp.transport_error);
    }
    if (resp.status == 401) {
        // Token may be expired/revoked — refresh once and retry once.
        token_ = SalesforceAuth::ExchangeRefreshToken(config_, client_);
        resp = do_req();
        if (!resp.transport_ok) {
            throw IOException("salesforce: request to %s failed to reach the server (%s).", path,
                              resp.transport_error);
        }
        if (resp.status == 401) {
            throw IOException(
                "salesforce: authentication failed (HTTP 401) after refreshing the token.");
        }
    }
    return resp;
}

string SalesforceSession::AuthorizedGet(const string &path) {
    HttpResponse resp = AuthorizedSend(false, path, "");
    if (resp.status == 200) {
        return resp.body;
    }
    // Salesforce REST errors come back as [{"errorCode":"...","message":"..."}].
    // Surface only those fields — never the body wholesale, never a secret.
    string code = sfjson::GetString(resp.body, "errorCode");
    string msg = sfjson::GetString(resp.body, "message");
    if (code.empty()) {
        code = "error";
    }
    throw IOException("salesforce: request to %s failed (HTTP %d): %s%s%s.", path,
                      resp.status, code, msg.empty() ? "" : " - ", msg);
}

bool SalesforceSession::TryEstimateCount(const string &count_soql, int64_t &out_rows) {
    try {
        HttpResponse resp = AuthorizedSend(false, QueryPath(count_soql), "");
        if (resp.status != 200) {
            return false;
        }
        bool found = false, is_null = false;
        string raw;
        sfjson::GetValue(resp.body, "totalSize", raw, found, is_null);
        if (!found || is_null) {
            return false;
        }
        out_rows = sfjson::GetInt(resp.body, "totalSize", -1);
        return out_rows >= 0;
    } catch (...) {
        return false; // never block a query on the estimator
    }
}

SalesforceDescribe SalesforceSession::Describe(const string &object) {
    string path =
        "/services/data/" + config_.api_version + "/sobjects/" + object + "/describe";
    string body = AuthorizedGet(path);
    SalesforceDescribe d = ParseDescribe(body, object);
    // Authoritative: we described /sobjects/<object>/describe, so the object
    // name IS `object`. Do not trust a name scraped from the JSON — a naive
    // reader can pick a nested "name" (action override / child relationship)
    // instead of the top-level one.
    d.object_name = object;
    return d;
}

// JSON-escape a string for embedding the SOQL in the job-create body.
static string JsonEscape(const string &s) {
    string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

// Surface a Bulk error secret-free: HTTP status + Salesforce errorCode/message.
[[noreturn]] static void ThrowBulkError(const char *stage, const HttpResponse &r) {
    string code = sfjson::GetString(r.body, "errorCode");
    string msg = sfjson::GetString(r.body, "message");
    if (code.empty()) {
        code = "error";
    }
    throw IOException("salesforce bulk %s failed (HTTP %d): %s%s%s.", stage, r.status, code,
                      msg.empty() ? "" : " - ", msg);
}

// Parse one CSV result page; first page sets the header, later pages repeat it.
static void AppendCsvPage(const string &csv, SalesforceBulkResult &result) {
    auto rows = sfcsv::Parse(csv);
    if (rows.empty()) {
        return;
    }
    size_t start = 1; // row 0 is the header on every page
    if (result.columns.empty()) {
        result.columns = rows[0];
    }
    for (size_t r = start; r < rows.size(); r++) {
        result.rows.push_back(std::move(rows[r]));
    }
}

SalesforceBulkResult SalesforceSession::BulkQuery(const string &soql) {
    const string base = "/services/data/" + config_.api_version + "/jobs/query";

    // 1) create the query job.
    string create_body = "{\"operation\":\"query\",\"query\":\"" + JsonEscape(soql) +
                         "\",\"contentType\":\"CSV\",\"lineEnding\":\"LF\"}";
    SetLastBulkCreateBody(create_body); // DEBUG/TEST diagnostic; no secret in body
    HttpResponse cr = AuthorizedSend(true, base, create_body);
    if (cr.status < 200 || cr.status >= 300) {
        ThrowBulkError("job create", cr);
    }
    string job_id = sfjson::GetString(cr.body, "id");
    if (job_id.empty()) {
        throw IOException("salesforce bulk: job create returned no id.");
    }
    const string job_path = base + "/" + job_id;

    // 2) poll until JobComplete (bounded; short backoff between polls).
    constexpr int kMaxPolls = 600;
    for (int i = 0;; i++) {
        HttpResponse st = AuthorizedSend(false, job_path, "");
        if (st.status < 200 || st.status >= 300) {
            ThrowBulkError("job status", st);
        }
        string state = sfjson::GetString(st.body, "state");
        if (state == "JobComplete") {
            break;
        }
        if (state == "Failed" || state == "Aborted") {
            string msg = sfjson::GetString(st.body, "errorMessage");
            throw IOException("salesforce bulk: job %s%s%s.", state, msg.empty() ? "" : " - ", msg);
        }
        if (i >= kMaxPolls) {
            throw IOException("salesforce bulk: job did not complete after %d polls.", kMaxPolls);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // 3) download CSV result pages, following Sforce-Locator.
    SalesforceBulkResult result;
    string path = job_path + "/results";
    std::unordered_set<string> seen;
    constexpr idx_t kMaxPages = 1000000;
    idx_t pages = 0;
    while (true) {
        HttpResponse rs = AuthorizedSend(false, path, "");
        if (rs.status < 200 || rs.status >= 300) {
            ThrowBulkError("job results", rs);
        }
        AppendCsvPage(rs.body, result);
        string loc = rs.GetHeader("Sforce-Locator");
        if (loc.empty() || loc == "null") {
            break;
        }
        if (++pages >= kMaxPages) {
            throw IOException("salesforce bulk: exceeded the maximum result page count.");
        }
        if (!seen.insert(loc).second) {
            throw IOException("salesforce bulk: result pagination loop (locator repeated).");
        }
        path = job_path + "/results?locator=" + loc;
    }
    return result;
}

vector<string> SalesforceSession::GlobalDescribe() {
    string path = "/services/data/" + config_.api_version + "/sobjects";
    string body = AuthorizedGet(path);
    vector<string> names;
    for (auto &obj : sfjson::GetObjectArray(body, "sobjects")) {
        if (!sfjson::GetBool(obj, "queryable", false)) {
            continue; // list only objects we can SOQL-query
        }
        string name = sfjson::GetString(obj, "name");
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

string SalesforceSession::QueryPath(const string &soql) const {
    return "/services/data/" + config_.api_version + "/query?q=" + UrlEncodeComponent(soql);
}

SalesforceQueryPage SalesforceSession::FetchPage(const string &path) {
    string body = AuthorizedGet(path);
    SalesforceQueryPage pg;
    for (auto &rec : sfjson::GetObjectArray(body, "records")) {
        pg.records.push_back(std::move(rec));
    }
    pg.done = sfjson::GetBool(body, "done", true);
    pg.next_path = sfjson::GetString(body, "nextRecordsUrl"); // opaque; used verbatim
    return pg;
}

SalesforceQueryResult SalesforceSession::Query(const string &soql) {
    // Eager: fetch every page. Defensive ceiling bounds a misbehaving cursor.
    constexpr idx_t kMaxPages = 1000000;

    SalesforceQueryResult result;
    string path = QueryPath(soql);
    std::unordered_set<string> seen_cursors;
    while (true) {
        SalesforceQueryPage pg = FetchPage(path);
        for (auto &rec : pg.records) {
            result.records.push_back(std::move(rec));
        }
        result.page_count++;

        if (pg.done || pg.next_path.empty()) {
            break;
        }
        if (result.page_count >= kMaxPages) {
            throw IOException(
                "salesforce query aborted: exceeded the maximum page count (%llu).",
                static_cast<unsigned long long>(kMaxPages));
        }
        if (!seen_cursors.insert(pg.next_path).second) {
            throw IOException(
                "salesforce query aborted: pagination loop detected (nextRecordsUrl "
                "repeated).");
        }
        path = pg.next_path;
    }
    return result;
}

} // namespace duckdb

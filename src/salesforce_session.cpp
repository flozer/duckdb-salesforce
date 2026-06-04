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
#include "salesforce_types.hpp"
#include "salesforce_url.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

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

// Return the balanced "{...}" object that is the value of `key`, or "" if `key`
// is absent or its value is not an object. Used to scope nested reads (e.g. the
// Max/Remaining inside one /limits entry) so a whole-string scan can't cross
// into a sibling object.
static string ExtractObject(const string &json, const string &key) {
    size_t i = sfjson::FindValue(json, key);
    if (i == string::npos || i >= json.size() || json[i] != '{') {
        return "";
    }
    size_t start = i;
    int depth = 0;
    bool in_str = false;
    for (; i < json.size(); i++) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) {
                return json.substr(start, i - start + 1);
            }
        }
    }
    return "";
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
    // Daily API allocation exhausted (HTTP 403 errorCode REQUEST_LIMIT_EXCEEDED)
    // is TERMINAL — it resets at org midnight, so retrying/backoff is pointless.
    // Distinct from HTTP 429 (short-term rate limit), which the transport retries.
    // Surface a clear, secret-free error (no body/bearer).
    if (resp.status >= 400 &&
        sfjson::GetString(resp.body, "errorCode") == "REQUEST_LIMIT_EXCEEDED") {
        throw IOException(
            "salesforce: daily API request limit exhausted (REQUEST_LIMIT_EXCEEDED). This "
            "resets at the org's midnight and is not retried. Reduce usage, raise the org "
            "limit, or wait for the reset.");
    }
    return resp;
}

SalesforceQuotaSnapshot SalesforceSession::QueryLimits() {
    SalesforceQuotaSnapshot s;
    try {
        HttpResponse resp =
            AuthorizedSend(false, "/services/data/" + config_.api_version + "/limits", "");
        if (resp.status != 200) {
            return s; // available stays false
        }
        // /limits => { "DailyApiRequests": {"Max":N,"Remaining":M}, ... }. Scope
        // the Max/Remaining reads to each named sub-object (GetInt scans whole
        // string, so we must extract the object first).
        string api = ExtractObject(resp.body, "DailyApiRequests");
        if (!api.empty()) {
            s.api_max = sfjson::GetInt(api, "Max", -1);
            s.api_remaining = sfjson::GetInt(api, "Remaining", -1);
        }
        string bulk = ExtractObject(resp.body, "DailyBulkV2QueryJobs");
        if (!bulk.empty()) {
            s.bulk_max = sfjson::GetInt(bulk, "Max", -1);
            s.bulk_remaining = sfjson::GetInt(bulk, "Remaining", -1);
        }
        s.available = (s.api_max >= 0 && s.api_remaining >= 0);
    } catch (...) {
        // never let the governance endpoint break a real query
    }
    return s;
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

bool SalesforceSession::ToolingDescribe(const vector<string> &objects,
                                        std::unordered_map<string, SalesforceDescribe> &out) {
    if (objects.empty()) {
        return true;
    }
    // Bound the IN list so the Tooling WHERE stays small ("few queries").
    constexpr size_t kChunk = 100;
    for (size_t start = 0; start < objects.size(); start += kChunk) {
        size_t end = std::min(objects.size(), start + kChunk);
        string in_list;
        for (size_t i = start; i < end; i++) {
            // Object API names are [A-Za-z0-9_], but escape defensively.
            string name = objects[i];
            string esc;
            for (char c : name) {
                if (c == '\\' || c == '\'') {
                    esc.push_back('\\');
                }
                esc.push_back(c);
            }
            in_list += (i > start ? ",'" : "'");
            in_list += esc;
            in_list += "'";
        }
        string soql = "SELECT EntityDefinition.QualifiedApiName, QualifiedApiName, DataType, "
                      "IsCompound, IsFilterable FROM FieldDefinition WHERE "
                      "EntityDefinition.QualifiedApiName IN (" +
                      in_list + ")";
        string path =
            "/services/data/" + config_.api_version + "/tooling/query?q=" + UrlEncodeComponent(soql);

        std::unordered_set<string> seen;
        int guard = 0;
        while (true) {
            IncToolingCalls(); // one Tooling HTTP query (chunk page)
            HttpResponse resp = AuthorizedSend(false, path, "");
            if (resp.status != 200) {
                return false; // caller falls back to REST describe entirely
            }
            for (auto &rec : sfjson::GetObjectArray(resp.body, "records")) {
                // Relationship field comes back nested:
                // {"EntityDefinition":{"QualifiedApiName":...},"QualifiedApiName":...}.
                // The nested object holds the OBJECT name; the top-level
                // QualifiedApiName holds the FIELD name. A flat key reader would
                // pick the nested one first, so read the object name from the
                // nested block and the field keys from the record with that
                // block stripped out.
                string ent = ExtractObject(rec, "EntityDefinition");
                string obj = ent.empty() ? string() : sfjson::GetString(ent, "QualifiedApiName");
                string rec_flat = rec;
                size_t ep = rec_flat.find("\"EntityDefinition\"");
                if (ep != string::npos) {
                    size_t br = rec_flat.find('{', ep);
                    if (br != string::npos) {
                        int depth = 0;
                        size_t i = br;
                        for (; i < rec_flat.size(); i++) {
                            if (rec_flat[i] == '{') {
                                depth++;
                            } else if (rec_flat[i] == '}' && --depth == 0) {
                                i++;
                                break;
                            }
                        }
                        rec_flat.erase(ep, i - ep);
                    }
                }
                string fname = sfjson::GetString(rec_flat, "QualifiedApiName");
                if (obj.empty() || fname.empty()) {
                    continue;
                }
                if (sfjson::GetBool(rec_flat, "IsCompound", false)) {
                    continue; // compound fields are not SOQL-selectable — drop
                }
                SalesforceField f;
                f.name = fname;
                f.sf_type = sfjson::GetString(rec_flat, "DataType");
                bool ok = true;
                f.duckdb_type = MapToolingDataType(f.sf_type, &ok);
                f.unknown_type = !ok; // ambiguous -> caller falls back per object
                // verify-then-conservative: only filterable if Tooling says so.
                f.filterable = sfjson::GetBool(rec_flat, "IsFilterable", false);
                auto &desc = out[StringUtil::Lower(obj)];
                if (desc.object_name.empty()) {
                    desc.object_name = obj;
                }
                desc.fields.push_back(std::move(f));
            }
            bool done = sfjson::GetBool(resp.body, "done", true);
            string next = sfjson::GetString(resp.body, "nextRecordsUrl");
            if (done || next.empty()) {
                break;
            }
            if (++guard > 100000 || !seen.insert(next).second) {
                break; // pagination guard
            }
            path = next;
        }
    }
    return true;
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

string SalesforceSession::BulkStartJob(const string &soql) {
    const string base = "/services/data/" + config_.api_version + "/jobs/query";

    // 1) create the query job. operation "queryAll" (#v0.9 §1) also returns
    // archived + soft-deleted records, mirroring the REST queryAll endpoint.
    const char *op = query_all_ ? "queryAll" : "query";
    string create_body = "{\"operation\":\"" + string(op) + "\",\"query\":\"" + JsonEscape(soql) +
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

    // 2) poll until JobComplete (bounded; short backoff). The server must finish
    // the job before any results exist — only the result DOWNLOAD is lazy (#8).
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

    // Base /results path; the scan fetches pages lazily from here.
    return job_path + "/results";
}

bool SalesforceSession::TryMinMaxId(const string &object, const string &where, string &min_id,
                                    string &max_id) {
    try {
        string soql = "SELECT MIN(Id) mn, MAX(Id) mx FROM " + object;
        if (!where.empty()) {
            soql += " WHERE " + where;
        }
        HttpResponse resp = AuthorizedSend(false, QueryPath(soql), "");
        if (resp.status != 200) {
            return false;
        }
        auto records = sfjson::GetObjectArray(resp.body, "records");
        if (records.empty()) {
            return false;
        }
        bool fm = false, nm = false, fx = false, nx = false;
        string mn, mx;
        sfjson::GetValue(records[0], "mn", mn, fm, nm);
        sfjson::GetValue(records[0], "mx", mx, fx, nx);
        if (!fm || nm || !fx || nx || mn.empty() || mx.empty()) {
            return false; // empty object / null aggregate -> single chunk
        }
        min_id = mn;
        max_id = mx;
        return min_id < max_id;
    } catch (...) {
        return false;
    }
}

SalesforceBulkPage SalesforceSession::BulkFetchResultPage(const string &path) {
    HttpResponse rs = AuthorizedSend(false, path, "");
    if (rs.status < 200 || rs.status >= 300) {
        ThrowBulkError("job results", rs);
    }
    SalesforceBulkPage page;
    auto rows = sfcsv::Parse(rs.body);
    if (!rows.empty()) {
        page.columns = std::move(rows[0]); // row 0 is the header on every page
        for (size_t r = 1; r < rows.size(); r++) {
            page.rows.push_back(std::move(rows[r]));
        }
    }
    string loc = rs.GetHeader("Sforce-Locator");
    if (loc != "null") {
        page.next_locator = loc;
    }
    return page;
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
    // queryAll (#v0.9 §1) also returns archived + soft-deleted (IsDeleted=true)
    // records. queryMore follows the returned nextRecordsUrl either way.
    const char *endpoint = query_all_ ? "/queryAll?q=" : "/query?q=";
    return "/services/data/" + config_.api_version + endpoint + UrlEncodeComponent(soql);
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

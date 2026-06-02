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

#include "salesforce_url.hpp"

#include "duckdb/common/exception.hpp"

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

string SalesforceSession::AuthorizedGet(const string &path) {
    auto do_get = [&]() {
        HttpRequest req;
        req.url = TrimTrailingSlash(token_.instance_url) + path;
        req.headers = {{"Authorization", "Bearer " + token_.access_token},
                       {"Accept", "application/json"}};
        return client_.Get(req);
    };

    HttpResponse resp = do_get();
    if (!resp.transport_ok) {
        // path is not a secret; transport_error is generic.
        throw IOException("salesforce: GET %s failed to reach the server (%s).", path,
                          resp.transport_error);
    }

    if (resp.status == 401) {
        // Token may be expired/revoked — refresh once and retry once.
        token_ = SalesforceAuth::ExchangeRefreshToken(config_, client_);
        resp = do_get();
        if (!resp.transport_ok) {
            throw IOException("salesforce: GET %s failed to reach the server (%s).", path,
                              resp.transport_error);
        }
        if (resp.status == 401) {
            throw IOException(
                "salesforce: authentication failed (HTTP 401) after refreshing the token.");
        }
    }

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

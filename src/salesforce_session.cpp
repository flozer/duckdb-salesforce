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

#include "duckdb/common/exception.hpp"

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
    return ParseDescribe(body, object);
}

} // namespace duckdb

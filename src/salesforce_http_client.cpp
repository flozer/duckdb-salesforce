// Live HTTPS transport (issue #4) + client factory / scripted mock (issue #5).
//
// LiveHttpClient: real network client on vendored httplib + OpenSSL.
//   - HTTPS only; TLS server-certificate verification ALWAYS on (no insecure
//     build flag).
//   - transient retry with capped backoff for HTTP 429, 5xx, and connection
//     failures; certificate-verification failure is permanent (not retried).
//   - request body never logged; transport errors are generic and carry no
//     request data/secrets.
//
// ScriptedMockHttpClient: test double driven by the sf_mock_* settings so
// sqllogictest can exercise the token exchange (POST) and authenticated GET
// (describe), including the 401 -> refresh -> retry path, with no network.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "httplib.h"

#include "salesforce_http.hpp"

#include "duckdb/main/client_context.hpp"

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <wincrypt.h>
#endif

namespace duckdb {

namespace {

struct ParsedUrl {
    bool https = false;
    string host;
    int port = 443;
    string path;
};

static bool ParseUrl(const string &url, ParsedUrl &out) {
    auto scheme_end = url.find("://");
    if (scheme_end == string::npos) {
        return false;
    }
    string scheme = url.substr(0, scheme_end);
    out.https = (scheme == "https" || scheme == "HTTPS");
    string rest = url.substr(scheme_end + 3);
    auto slash = rest.find('/');
    string authority = (slash == string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == string::npos) ? "/" : rest.substr(slash);
    auto colon = authority.find(':');
    if (colon == string::npos) {
        out.host = authority;
        out.port = out.https ? 443 : 80;
    } else {
        out.host = authority.substr(0, colon);
        out.port = std::atoi(authority.substr(colon + 1).c_str());
    }
    return !out.host.empty();
}

static void ApplyTrustStore(httplib::SSLClient &cli) {
#ifdef _WIN32
    HCERTSTORE h = CertOpenSystemStoreW(0, L"ROOT");
    if (!h) {
        return;
    }
    X509_STORE *store = X509_STORE_new();
    PCCERT_CONTEXT ctx = nullptr;
    while ((ctx = CertEnumCertificatesInStore(h, ctx)) != nullptr) {
        const unsigned char *p = ctx->pbCertEncoded;
        X509 *x = d2i_X509(nullptr, &p, static_cast<long>(ctx->cbCertEncoded));
        if (x) {
            X509_STORE_add_cert(store, x);
            X509_free(x);
        }
    }
    CertCloseStore(h, 0);
    cli.set_ca_cert_store(store); // client takes ownership
#else
    SSL_CTX_set_default_verify_paths(cli.ssl_context());
#endif
}

static bool IsTransientStatus(int status) {
    return status == 429 || status >= 500;
}

enum class Method { GET, POST };

class LiveHttpClient final : public SalesforceHttpClient {
public:
    HttpResponse Post(const HttpRequest &request) override {
        return Send(Method::POST, request);
    }
    HttpResponse Get(const HttpRequest &request) override {
        return Send(Method::GET, request);
    }

private:
    static void Backoff(int attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
    }

    static HttpResponse TransportError(const string &msg) {
        HttpResponse r;
        r.transport_ok = false;
        r.transport_error = msg;
        return r;
    }

    HttpResponse Send(Method method, const HttpRequest &request) {
        ParsedUrl url;
        if (!ParseUrl(request.url, url) || !url.https) {
            return TransportError("URL must be an absolute https:// endpoint");
        }

        httplib::Headers headers;
        string content_type = "application/x-www-form-urlencoded";
        for (const auto &h : request.headers) {
            if (StringUtil::CIEquals(h.first, "Content-Type")) {
                content_type = h.second;
            } else {
                headers.emplace(h.first, h.second);
            }
        }

        constexpr int kMaxAttempts = 3;
        for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
            httplib::SSLClient cli(url.host, url.port);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(60, 0);
            cli.set_write_timeout(60, 0);
            cli.enable_server_certificate_verification(true); // never disabled
            ApplyTrustStore(cli);

            httplib::Result res =
                (method == Method::POST)
                    ? cli.Post(url.path, headers, request.body, content_type)
                    : cli.Get(url.path, headers);

            if (res) {
                if (IsTransientStatus(res->status) && attempt < kMaxAttempts) {
                    Backoff(attempt);
                    continue;
                }
                HttpResponse r;
                r.transport_ok = true;
                r.status = res->status;
                r.body = res->body;
                for (auto &h : res->headers) {
                    r.headers.emplace_back(h.first, h.second);
                }
                return r;
            }

            auto err = res.error();
            if (err == httplib::Error::SSLServerVerification) {
                return TransportError("TLS certificate verification failed");
            }
            if (attempt < kMaxAttempts) {
                Backoff(attempt);
                continue;
            }
            return TransportError(httplib::to_string(err));
        }
        return TransportError("exhausted retries");
    }
};

// Test double. Post() returns the mocked token-endpoint response (initial
// exchange + every 401 refresh). Get() is routed by URL into two independent
// scripted sequences — one for describe (.../describe) and one for query
// (.../query, queryMore) — each a list of (status, body) responses that
// advance per call (last entry repeats once exhausted). The split lets a
// single SQL flow drive ATTACH-exchange -> describe -> query even though each
// operation builds its own client instance.
class ScriptedMockHttpClient final : public SalesforceHttpClient {
public:
    struct BulkMock {
        int create_status = 200;
        string create_body;
        vector<int> status_statuses;
        vector<string> status_bodies;
        vector<int> results_statuses;
        vector<string> results_bodies;
        vector<string> results_locators;
    };

    ScriptedMockHttpClient(int token_status, string token_body, vector<int> describe_statuses,
                           vector<string> describe_bodies, vector<int> query_statuses,
                           vector<string> query_bodies, vector<int> global_statuses,
                           vector<string> global_bodies, vector<int> count_statuses,
                           vector<string> count_bodies, vector<int> limits_statuses,
                           vector<string> limits_bodies, vector<int> tooling_statuses,
                           vector<string> tooling_bodies, BulkMock bulk)
        : token_status_(token_status), token_body_(std::move(token_body)),
          describe_statuses_(std::move(describe_statuses)),
          describe_bodies_(std::move(describe_bodies)),
          query_statuses_(std::move(query_statuses)),
          query_bodies_(std::move(query_bodies)),
          global_statuses_(std::move(global_statuses)),
          global_bodies_(std::move(global_bodies)),
          count_statuses_(std::move(count_statuses)),
          count_bodies_(std::move(count_bodies)),
          limits_statuses_(std::move(limits_statuses)),
          limits_bodies_(std::move(limits_bodies)),
          tooling_statuses_(std::move(tooling_statuses)),
          tooling_bodies_(std::move(tooling_bodies)), bulk_(std::move(bulk)) {
    }

    HttpResponse Post(const HttpRequest &request) override {
        HttpResponse r;
        r.transport_ok = true;
        if (request.url.find("/jobs/query") != string::npos) {
            r.status = bulk_.create_status; // Bulk job create
            r.body = bulk_.create_body;
        } else {
            r.status = token_status_; // OAuth token exchange / refresh
            r.body = token_body_;
        }
        return r;
    }

    HttpResponse Get(const HttpRequest &request) override {
        // Tooling API fast schema (#v0.6 §6): .../tooling/query -> tooling seq.
        if (request.url.find("/tooling/") != string::npos) {
            return Step(tooling_statuses_, tooling_bodies_, tooling_index_);
        }
        // Bulk: .../jobs/query/<id>/results vs .../jobs/query/<id> (status).
        if (request.url.find("/jobs/query") != string::npos) {
            if (request.url.find("/results") != string::npos) {
                HttpResponse r = Step(bulk_.results_statuses, bulk_.results_bodies, bulk_results_index_);
                string loc = (bulk_.results_locators.empty())
                                 ? string()
                                 : bulk_.results_locators[bulk_results_index_ - 1 < bulk_.results_locators.size()
                                                              ? bulk_results_index_ - 1
                                                              : bulk_.results_locators.size() - 1];
                if (!loc.empty()) {
                    r.headers.emplace_back("Sforce-Locator", loc);
                }
                return r;
            }
            return Step(bulk_.status_statuses, bulk_.status_bodies, bulk_status_index_);
        }
        // describe URL (.../sobjects/<obj>/describe) contains both tokens, so
        // check /describe first; then /sobjects (global describe); else query.
        if (request.url.find("/describe") != string::npos) {
            return Step(describe_statuses_, describe_bodies_, describe_index_);
        }
        if (request.url.find("/sobjects") != string::npos) {
            return Step(global_statuses_, global_bodies_, global_index_);
        }
        // Quota governor (#v0.4): GET .../limits -> limits sequence.
        if (request.url.find("/limits") != string::npos) {
            return Step(limits_statuses_, limits_bodies_, limits_index_);
        }
        // Auto-transport probe (#v0.3 §2): SELECT COUNT() ... -> count sequence.
        // "COUNT" survives URL-encoding (letters only), so match it before the
        // generic data-query branch.
        if (request.url.find("COUNT") != string::npos) {
            return Step(count_statuses_, count_bodies_, count_index_);
        }
        return Step(query_statuses_, query_bodies_, query_index_);
    }

private:
    static HttpResponse Step(const vector<int> &statuses, const vector<string> &bodies,
                             size_t &index) {
        HttpResponse r;
        r.transport_ok = true;
        r.status = statuses.empty() ? 200 : statuses[index < statuses.size() ? index : statuses.size() - 1];
        r.body = bodies.empty() ? string() : bodies[index < bodies.size() ? index : bodies.size() - 1];
        index++;
        return r;
    }

    int token_status_;
    string token_body_;
    vector<int> describe_statuses_;
    vector<string> describe_bodies_;
    vector<int> query_statuses_;
    vector<string> query_bodies_;
    vector<int> global_statuses_;
    vector<string> global_bodies_;
    vector<int> count_statuses_;
    vector<string> count_bodies_;
    vector<int> limits_statuses_;
    vector<string> limits_bodies_;
    vector<int> tooling_statuses_;
    vector<string> tooling_bodies_;
    BulkMock bulk_;
    size_t describe_index_ = 0;
    size_t query_index_ = 0;
    size_t global_index_ = 0;
    size_t count_index_ = 0;
    size_t limits_index_ = 0;
    size_t tooling_index_ = 0;
    size_t bulk_status_index_ = 0;
    size_t bulk_results_index_ = 0;
};

// Split a string on a multi-char sentinel. An empty input yields one empty
// element (a single empty response body).
static vector<string> SplitOn(const string &s, const string &sep) {
    vector<string> out;
    size_t prev = 0, pos;
    while ((pos = s.find(sep, prev)) != string::npos) {
        out.push_back(s.substr(prev, pos - prev));
        prev = pos + sep.size();
    }
    out.push_back(s.substr(prev));
    return out;
}

// Parse a comma-separated list of integers (e.g. "200,401,200").
static vector<int> ParseIntCsv(const string &s) {
    vector<int> out;
    for (auto &part : SplitOn(s, ",")) {
        string t;
        for (char c : part) {
            if (c != ' ') {
                t.push_back(c);
            }
        }
        if (!t.empty()) {
            try {
                out.push_back(std::stoi(t));
            } catch (...) {
            }
        }
    }
    return out;
}

static int64_t SettingInt(ClientContext &ctx, const char *key, int64_t dflt) {
    Value v;
    if (ctx.TryGetCurrentSetting(key, v) && !v.IsNull()) {
        return v.GetValue<int64_t>();
    }
    return dflt;
}

static string SettingStr(ClientContext &ctx, const char *key) {
    Value v;
    if (ctx.TryGetCurrentSetting(key, v) && !v.IsNull()) {
        return v.ToString();
    }
    return "";
}

} // namespace

unique_ptr<SalesforceHttpClient> CreateLiveHttpClient() {
    return make_uniq_base<SalesforceHttpClient, LiveHttpClient>();
}

unique_ptr<SalesforceHttpClient> BuildHttpClientForContext(ClientContext &context) {
    auto token_status = SettingInt(context, "sf_mock_token_status", 0);
    if (token_status != 0) {
        // "|~|" separates page bodies; chosen to never occur in JSON fixtures.
        vector<int> d_status = ParseIntCsv(SettingStr(context, "sf_mock_describe_status"));
        if (d_status.empty()) {
            d_status.push_back(200);
        }
        vector<string> d_body = SplitOn(SettingStr(context, "sf_mock_describe_body"), "|~|");
        vector<int> q_status = ParseIntCsv(SettingStr(context, "sf_mock_query_status"));
        if (q_status.empty()) {
            q_status.push_back(200);
        }
        vector<string> q_body = SplitOn(SettingStr(context, "sf_mock_query_body"), "|~|");
        vector<int> g_status = ParseIntCsv(SettingStr(context, "sf_mock_sobjects_status"));
        if (g_status.empty()) {
            g_status.push_back(200);
        }
        vector<string> g_body = SplitOn(SettingStr(context, "sf_mock_sobjects_body"), "|~|");
        vector<int> c_status = ParseIntCsv(SettingStr(context, "sf_mock_count_status"));
        if (c_status.empty()) {
            c_status.push_back(200);
        }
        vector<string> c_body = SplitOn(SettingStr(context, "sf_mock_count_body"), "|~|");
        vector<int> l_status = ParseIntCsv(SettingStr(context, "sf_mock_limits_status"));
        if (l_status.empty()) {
            l_status.push_back(200);
        }
        vector<string> l_body = SplitOn(SettingStr(context, "sf_mock_limits_body"), "|~|");
        vector<int> t_status = ParseIntCsv(SettingStr(context, "sf_mock_tooling_status"));
        if (t_status.empty()) {
            t_status.push_back(200);
        }
        vector<string> t_body = SplitOn(SettingStr(context, "sf_mock_tooling_body"), "|~|");

        ScriptedMockHttpClient::BulkMock bulk;
        bulk.create_status = static_cast<int>(SettingInt(context, "sf_mock_bulk_create_status", 200));
        bulk.create_body = SettingStr(context, "sf_mock_bulk_create_body");
        bulk.status_statuses = ParseIntCsv(SettingStr(context, "sf_mock_bulk_status_code"));
        if (bulk.status_statuses.empty()) {
            bulk.status_statuses.push_back(200);
        }
        bulk.status_bodies = SplitOn(SettingStr(context, "sf_mock_bulk_status_body"), "|~|");
        bulk.results_statuses = ParseIntCsv(SettingStr(context, "sf_mock_bulk_results_status"));
        if (bulk.results_statuses.empty()) {
            bulk.results_statuses.push_back(200);
        }
        bulk.results_bodies = SplitOn(SettingStr(context, "sf_mock_bulk_results_body"), "|~|");
        bulk.results_locators = SplitOn(SettingStr(context, "sf_mock_bulk_results_locator"), ",");

        return make_uniq_base<SalesforceHttpClient, ScriptedMockHttpClient>(
            static_cast<int>(token_status), SettingStr(context, "sf_mock_token_body"),
            std::move(d_status), std::move(d_body), std::move(q_status), std::move(q_body),
            std::move(g_status), std::move(g_body), std::move(c_status), std::move(c_body),
            std::move(l_status), std::move(l_body), std::move(t_status), std::move(t_body),
            std::move(bulk));
    }
    return CreateLiveHttpClient();
}

} // namespace duckdb

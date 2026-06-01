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

// Test double. Post() returns the mocked token-endpoint response; Get() returns
// the mocked describe response, optionally forcing a 401 on the first GET to
// drive the 401 -> refresh -> retry path.
class ScriptedMockHttpClient final : public SalesforceHttpClient {
public:
    ScriptedMockHttpClient(int token_status, string token_body, int describe_status,
                           string describe_body, bool first_get_401)
        : token_status_(token_status), token_body_(std::move(token_body)),
          describe_status_(describe_status), describe_body_(std::move(describe_body)),
          first_get_401_(first_get_401) {
    }

    HttpResponse Post(const HttpRequest & /*request*/) override {
        HttpResponse r;
        r.transport_ok = true;
        r.status = token_status_;
        r.body = token_body_;
        return r;
    }

    HttpResponse Get(const HttpRequest & /*request*/) override {
        HttpResponse r;
        r.transport_ok = true;
        if (first_get_401_ && get_count_ == 0) {
            r.status = 401;
        } else {
            r.status = describe_status_;
            r.body = describe_body_;
        }
        get_count_++;
        return r;
    }

private:
    int token_status_;
    string token_body_;
    int describe_status_;
    string describe_body_;
    bool first_get_401_;
    int get_count_ = 0;
};

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

static bool SettingBool(ClientContext &ctx, const char *key, bool dflt) {
    Value v;
    if (ctx.TryGetCurrentSetting(key, v) && !v.IsNull()) {
        return v.GetValue<bool>();
    }
    return dflt;
}

} // namespace

unique_ptr<SalesforceHttpClient> CreateLiveHttpClient() {
    return make_uniq_base<SalesforceHttpClient, LiveHttpClient>();
}

unique_ptr<SalesforceHttpClient> BuildHttpClientForContext(ClientContext &context) {
    auto token_status = SettingInt(context, "sf_mock_token_status", 0);
    if (token_status != 0) {
        return make_uniq_base<SalesforceHttpClient, ScriptedMockHttpClient>(
            static_cast<int>(token_status), SettingStr(context, "sf_mock_token_body"),
            static_cast<int>(SettingInt(context, "sf_mock_describe_status", 200)),
            SettingStr(context, "sf_mock_describe_body"),
            SettingBool(context, "sf_mock_describe_first401", false));
    }
    return CreateLiveHttpClient();
}

} // namespace duckdb

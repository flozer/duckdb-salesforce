// Live HTTPS transport (issue #4).
//
// Real network client behind the SalesforceHttpClient interface, built on the
// vendored httplib + OpenSSL. Guarantees:
//   - HTTPS only; TLS server-certificate verification is ALWAYS on. There is
//     no insecure / verify=false build flag.
//   - the request body (which may carry secrets) is never logged;
//   - transient failures (connection/TLS-handshake transport errors, HTTP 429
//     and 5xx) are retried with capped backoff; non-transient failures
//     (including TLS certificate verification failure) are not retried;
//   - transport error text is generic (httplib error category) and never
//     contains the request body.
//
// CI never reaches this code: tests inject MockHttpClient through the ATTACH
// mock hook. A real exchange is exercised only by the gated manual test.

// httplib pulls in platform socket + (on Windows) <windows.h>; include it
// first and keep NOMINMAX so it cannot clobber std::min/std::max used by duckdb.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "httplib.h"

#include "salesforce_http.hpp"

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

// Load trusted CA roots into the client so verification has an anchor set.
// Windows: the system ROOT store. Elsewhere: OpenSSL's default verify paths.
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

class LiveHttpClient final : public SalesforceHttpClient {
public:
    HttpResponse Post(const HttpRequest &request) override {
        ParsedUrl url;
        if (!ParseUrl(request.url, url) || !url.https) {
            HttpResponse r;
            r.transport_ok = false;
            r.transport_error = "URL must be an absolute https:// endpoint";
            return r;
        }

        // Split out Content-Type (httplib takes it as a separate argument).
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

            auto res = cli.Post(url.path, headers, request.body, content_type);

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

            // No HTTP response produced. Certificate-verification failures are
            // permanent — do not retry or mask them as transient.
            auto err = res.error();
            if (err == httplib::Error::SSLServerVerification) {
                HttpResponse r;
                r.transport_ok = false;
                r.transport_error = "TLS certificate verification failed";
                return r;
            }
            if (attempt < kMaxAttempts) {
                Backoff(attempt);
                continue;
            }
            HttpResponse r;
            r.transport_ok = false;
            // httplib error category text is generic (e.g. "Connection") and
            // contains no request data.
            r.transport_error = httplib::to_string(err);
            return r;
        }

        HttpResponse r;
        r.transport_ok = false;
        r.transport_error = "exhausted retries";
        return r;
    }

private:
    static void Backoff(int attempt) {
        // 200ms, 400ms, ... — bounded, only for transient failures.
        std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
    }
};

} // namespace

unique_ptr<SalesforceHttpClient> CreateLiveHttpClient() {
    return make_uniq_base<SalesforceHttpClient, LiveHttpClient>();
}

} // namespace duckdb

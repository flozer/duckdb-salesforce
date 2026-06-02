#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Minimal HTTP abstraction. The OAuth exchanger (#3) depends only on this
// interface so it can be unit-tested with a mock — CI never calls Salesforce.
// The concrete live HTTPS transport (TLS verification, retry/backoff, log
// masking) is issue #4 and plugs in behind the same interface.

struct HttpRequest {
    string url;
    vector<std::pair<string, string>> headers;
    string body; // request body — NEVER logged (may contain secrets)
};

struct HttpResponse {
    // transport_ok=false means the request never produced an HTTP response
    // (DNS/connection/TLS failure). transport_error is a short, secret-free
    // description in that case.
    bool transport_ok = false;
    int status = 0;
    string body;
    string transport_error;
    // Response headers (e.g. Bulk API 2.0 "Sforce-Locator"). Request headers
    // such as Authorization are never stored here.
    vector<std::pair<string, string>> headers;

    // Case-insensitive header lookup; "" if absent.
    string GetHeader(const string &name) const {
        for (auto &h : headers) {
            if (StringUtil::CIEquals(h.first, name)) {
                return h.second;
            }
        }
        return "";
    }
};

class SalesforceHttpClient {
public:
    virtual ~SalesforceHttpClient() = default;
    virtual HttpResponse Post(const HttpRequest &request) = 0;
    virtual HttpResponse Get(const HttpRequest &request) = 0;
};

// Test double: returns a canned status+body without any network. Header-only
// so both tests (via the ATTACH mock hook) and unit code can build it.
class MockHttpClient final : public SalesforceHttpClient {
public:
    MockHttpClient(int status, string body) : status_(status), body_(std::move(body)) {
    }
    HttpResponse Post(const HttpRequest & /*request*/) override {
        return Make();
    }
    HttpResponse Get(const HttpRequest & /*request*/) override {
        return Make();
    }

private:
    HttpResponse Make() const {
        HttpResponse r;
        r.transport_ok = true;
        r.status = status_;
        r.body = body_;
        return r;
    }
    int status_;
    string body_;
};

// Build the HTTP client for an ATTACH / function call: the live transport, or
// a scripted mock when the sf_mock_* test settings are active.
unique_ptr<SalesforceHttpClient> BuildHttpClientForContext(ClientContext &context);

// Live HTTPS client factory (#4): httplib + OpenSSL transport with TLS
// server-certificate verification always ON (no insecure bypass), transient
// retry/backoff, and no request-body logging. CI uses MockHttpClient instead;
// the live path is exercised only by the gated salesforce_oauth_live.test.
unique_ptr<SalesforceHttpClient> CreateLiveHttpClient();

} // namespace duckdb

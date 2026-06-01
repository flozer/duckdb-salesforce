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
};

class SalesforceHttpClient {
public:
    virtual ~SalesforceHttpClient() = default;
    virtual HttpResponse Post(const HttpRequest &request) = 0;
};

// Test double: returns a canned status+body without any network. Header-only
// so both tests (via the ATTACH mock hook) and unit code can build it.
class MockHttpClient final : public SalesforceHttpClient {
public:
    MockHttpClient(int status, string body) : status_(status), body_(std::move(body)) {
    }
    HttpResponse Post(const HttpRequest & /*request*/) override {
        HttpResponse r;
        r.transport_ok = true;
        r.status = status_;
        r.body = body_;
        return r;
    }

private:
    int status_;
    string body_;
};

// Live HTTPS client factory. In #3 the returned client throws a clear
// not-implemented error on Post(): the real httplib+OpenSSL transport (with
// TLS verification ON and no insecure bypass) is delivered in issue #4.
unique_ptr<SalesforceHttpClient> CreateLiveHttpClient();

} // namespace duckdb

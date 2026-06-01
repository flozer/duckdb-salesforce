// Live HTTPS client seam (issue #3 boundary).
//
// The OAuth exchanger (#3) is complete and unit-tested against the
// SalesforceHttpClient interface via a mock. The concrete network transport —
// httplib + OpenSSL, TLS verification ON with no insecure bypass, retry/backoff
// and Authorization-header log masking — is issue #4. Until then the live
// client throws a clear, secret-free message. Tests inject MockHttpClient
// through the ATTACH mock hook and never hit this path.

#include "salesforce_http.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

class NotImplementedLiveClient final : public SalesforceHttpClient {
public:
    HttpResponse Post(const HttpRequest & /*request*/) override {
        throw NotImplementedException(
            "duckdb-salesforce v0.1: the live HTTPS transport is implemented in "
            "issue #4 (HTTP transport + TLS + log masking). The OAuth exchange "
            "logic is ready; for testing, inject a mocked token response via the "
            "sf_mock_token_status / sf_mock_token_body settings.");
    }
};

} // namespace

unique_ptr<SalesforceHttpClient> CreateLiveHttpClient() {
    return make_uniq<NotImplementedLiveClient>();
}

} // namespace duckdb

// Salesforce ATTACH support — v0.1 scaffold stub.
//
// This file will eventually host the federated read-only catalog
// (SalesforceCatalog / SchemaEntry / TableEntry / TransactionManager),
// mirroring the structure of duckdb-firebird's firebird_storage.cpp.
//
// For the v0.1 scaffold cut (issue #1) it provides ONLY the StorageExtension
// registration so that `ATTACH '...' (TYPE salesforce)` resolves to a known
// type. The attach callback intentionally throws: real authentication and
// scanning are tracked by separate issues and must not be faked here.

#include "salesforce_storage.hpp"
#include "salesforce_config.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_http.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

// Test-only hook: when sf_mock_token_status != 0, the OAuth exchange uses a
// MockHttpClient returning that status + sf_mock_token_body instead of the live
// transport. This lets sqllogictest exercise the exchange without contacting
// Salesforce. It injects a canned RESPONSE only — it cannot read or exfiltrate
// the request secrets.
static unique_ptr<SalesforceHttpClient> BuildHttpClient(ClientContext &context) {
    Value status_v;
    if (context.TryGetCurrentSetting("sf_mock_token_status", status_v) &&
        !status_v.IsNull()) {
        auto status = status_v.GetValue<int64_t>();
        if (status != 0) {
            Value body_v;
            string body;
            if (context.TryGetCurrentSetting("sf_mock_token_body", body_v) &&
                !body_v.IsNull()) {
                body = body_v.ToString();
            }
            return make_uniq_base<SalesforceHttpClient, MockHttpClient>(
                static_cast<int>(status), std::move(body));
        }
    }
    return CreateLiveHttpClient();
}

static unique_ptr<Catalog>
SalesforceAttach(optional_ptr<StorageExtensionInfo> /*info*/,
                 ClientContext &context,
                 AttachedDatabase & /*db*/,
                 const string & /*name*/,
                 AttachInfo &attach_info,
                 AttachOptions & /*options*/) {
    // #2: parse + validate the connection config. Throws a clear,
    // secret-free BinderException on any missing/invalid field.
    SalesforceConfig config =
        SalesforceConfig::ParseAndValidate(attach_info.path, attach_info);

    // #3: OAuth 2.0 refresh-token exchange. Held in memory only; the request
    // body (with secrets) is never logged, and errors never echo secrets.
    auto http = BuildHttpClient(context);
    SalesforceTokenSet token = SalesforceAuth::ExchangeRefreshToken(config, *http);

    // Authentication succeeded. The catalog/scanner (#5-#9) will consume
    // `token` to issue SOQL queries; until then we stop here. We echo only the
    // non-secret instance_url to confirm the exchange — never the access_token.
    throw NotImplementedException(
        "duckdb-salesforce v0.1: authenticated org '%s' (instance_url '%s'), "
        "but table scanning is not implemented yet. Tracked in v0.1-readonly-rest "
        "(scan #5-#9).",
        config.org, token.instance_url);
}

static unique_ptr<TransactionManager>
SalesforceCreateTransactionManager(optional_ptr<StorageExtensionInfo> /*info*/,
                                   AttachedDatabase & /*db*/,
                                   Catalog & /*catalog*/) {
    // Never reached in v0.1: SalesforceAttach throws before any catalog or
    // transaction manager is constructed. The callback must nonetheless be
    // non-null — DuckDB (this pinned build) only dispatches ATTACH to a
    // storage extension when BOTH attach and create_transaction_manager are
    // set (see duckdb src/main/database.cpp CreateAttachedDatabase). Without
    // it, ATTACH silently falls back to opening the path as a DuckDB file.
    throw NotImplementedException(
        "duckdb-salesforce v0.1 scaffold: transaction manager not implemented.");
}

unique_ptr<StorageExtension> GetSalesforceStorageExtension() {
    auto ext = make_uniq<StorageExtension>();
    ext->attach = SalesforceAttach;
    ext->create_transaction_manager = SalesforceCreateTransactionManager;
    return ext;
}

} // namespace duckdb

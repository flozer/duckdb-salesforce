#define DUCKDB_EXTENSION_MAIN

#include "salesforce_extension.hpp"
#include "salesforce_storage.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_query.hpp"
#include "salesforce_value.hpp"
#include "salesforce_soql.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
    // Register the StorageExtension so DuckDB recognises
    //   ATTACH 'salesforce://…' AS sf (TYPE salesforce);
    // as a known storage type. v0.1 scaffold: the attach callback throws a
    // clear NotImplementedException (see salesforce_storage.cpp).
    //
    // StorageExtension::Register matches the registration path used by
    // duckdb-firebird. Verified against the pinned DuckDB v1.5.3 release:
    // ATTACH dispatch requires BOTH attach and create_transaction_manager to
    // be non-null (see salesforce_storage.cpp).
    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);
    auto storage_ext = GetSalesforceStorageExtension();
    StorageExtension::Register(config, "salesforce",
                               shared_ptr<StorageExtension>(storage_ext.release()));

    // salesforce_describe(object, client_id:=, client_secret:=, refresh_token:=,
    //   login_url:=, api_version:=) — introspect a single sObject's schema (#5).
    loader.RegisterFunction(GetSalesforceDescribeFunction());

    // salesforce_query(soql, client_id:=, ...) — paginated SOQL fetcher (#6),
    // returns raw JSON records. Typed scanning lands in #7/#8.
    loader.RegisterFunction(GetSalesforceQueryFunction());
    loader.RegisterFunction(GetSalesforceUrlEncodeFunction());

    // salesforce_decode(fields_json, records_json) — JSON record -> typed
    // DuckDB vectors (#7). Test/utility surface; the scan wires it in at #8.
    loader.RegisterFunction(GetSalesforceDecodeFunction());

    // salesforce_last_soql() — diagnostic: the SOQL the most recent scan
    // generated (projection + predicate pushdown). Used by tests.
    loader.RegisterFunction(GetSalesforceLastSoqlFunction());

    // Test-only hooks for the OAuth exchange (#3). When sf_mock_token_status is
    // non-zero, ATTACH uses a mock HTTP client returning that status and
    // sf_mock_token_body instead of the live transport, so sqllogictest can
    // exercise token exchange without contacting Salesforce. These inject a
    // canned RESPONSE only and never touch request secrets. Default (0)
    // disables the hook.
    config.AddExtensionOption(
        "sf_mock_token_status",
        "TEST ONLY. HTTP status for a mocked Salesforce token-endpoint "
        "response. 0 disables the mock and uses the live transport (default).",
        LogicalType::BIGINT, Value::BIGINT(0));
    config.AddExtensionOption(
        "sf_mock_token_body",
        "TEST ONLY. Response body paired with sf_mock_token_status.",
        LogicalType::VARCHAR, Value(""));
    // Mocked authenticated GETs are routed by URL into two scripted sequences:
    // describe (.../describe) and query (.../query). Each is a comma-separated
    // status list + '|~|'-separated bodies; the last entry repeats. Active only
    // when sf_mock_token_status != 0.
    config.AddExtensionOption("sf_mock_describe_status",
                              "TEST ONLY. Statuses for mocked describe GETs (e.g. '200', '401,200').",
                              LogicalType::VARCHAR, Value("200"));
    config.AddExtensionOption("sf_mock_describe_body",
                              "TEST ONLY. Bodies for mocked describe GETs ('|~|'-separated).",
                              LogicalType::VARCHAR, Value(""));
    config.AddExtensionOption("sf_mock_query_status",
                              "TEST ONLY. Statuses for mocked query GETs (e.g. '200,200').",
                              LogicalType::VARCHAR, Value("200"));
    config.AddExtensionOption("sf_mock_query_body",
                              "TEST ONLY. Bodies for mocked query GET pages ('|~|'-separated).",
                              LogicalType::VARCHAR, Value(""));
}

void SalesforceExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string SalesforceExtension::Name() {
    return "salesforce";
}

std::string SalesforceExtension::Version() const {
#ifdef EXT_VERSION_SALESFORCE
    return EXT_VERSION_SALESFORCE;
#else
    return "0.1.0";
#endif
}

} // namespace duckdb

// --- C entry points -----------------------------------------------------------
// DuckDB ≥ 1.4 calls the *_duckdb_cpp_init symbol declared by this macro.
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(salesforce, loader) {
    duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *salesforce_version() {
    return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif

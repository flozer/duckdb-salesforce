#define DUCKDB_EXTENSION_MAIN

#include "salesforce_extension.hpp"
#include "salesforce_storage.hpp"

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
    // duckdb-firebird. Verified against the pinned DuckDB submodule
    // (v1.5.2-6640-g0a8a19486d): ATTACH dispatch requires BOTH attach and
    // create_transaction_manager to be non-null (see salesforce_storage.cpp).
    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);
    auto storage_ext = GetSalesforceStorageExtension();
    StorageExtension::Register(config, "salesforce",
                               shared_ptr<StorageExtension>(storage_ext.release()));
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

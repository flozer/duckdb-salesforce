#pragma once

#include "duckdb.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

struct SalesforceConfig;
struct SalesforceTokenSet;
class ClientContext;

// Resolve an attached salesforce catalog by ATTACH alias and copy its in-memory
// credentials (config + token). Throws a clear, secret-free BinderException if
// `alias` is not an attached Salesforce catalog. Used by salesforce_aggregate().
void GetSalesforceCatalogCredentials(ClientContext &context, const string &alias,
                                     SalesforceConfig &cfg, SalesforceTokenSet &token);

// salesforce_refresh_metadata(catalog [, object]) — clear an attached salesforce
// catalog's in-memory metadata cache (#v1.3 §10). Empty object = global.
TableFunction GetSalesforceRefreshMetadataFunction();

// Returns the StorageExtension registered under the name "salesforce" so that
//
//     ATTACH 'salesforce://production' AS sf (TYPE salesforce);
//
// is recognised by DuckDB's catalog framework.
//
// v0.1 scaffold: the attach callback is a stub. It deliberately throws a clear
// NotImplementedException — authentication (issue #3) and the scanner
// (issues #5-#9) are not wired up yet. This entry point exists only so the
// extension loads and the ATTACH grammar resolves to a known storage type.
unique_ptr<StorageExtension> GetSalesforceStorageExtension();

} // namespace duckdb

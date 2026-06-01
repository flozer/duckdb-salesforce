#pragma once

#include "duckdb.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

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

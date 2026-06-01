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

#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

static unique_ptr<Catalog>
SalesforceAttach(optional_ptr<StorageExtensionInfo> /*info*/,
                 ClientContext & /*context*/,
                 AttachedDatabase & /*db*/,
                 const string & /*name*/,
                 AttachInfo & /*attach_info*/,
                 AttachOptions & /*options*/) {
    throw NotImplementedException(
        "duckdb-salesforce v0.1 is a scaffold: ATTACH, OAuth authentication "
        "and table scanning are not implemented yet. The extension loaded "
        "correctly. Progress is tracked in the v0.1-readonly-rest milestone "
        "(ATTACH parser #2, OAuth #3, HTTP transport #4, scan #5-#9).");
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

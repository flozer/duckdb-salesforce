#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"

namespace duckdb {

// Bind data for a catalog-driven sObject scan. Built by the table catalog
// entry's GetScanFunction; the scan runs the SOQL query (#6) and decodes the
// records (#7) into the output chunk. v0.1: always SELECT <all queryable
// fields> FROM <object> — no pushdown (#9).
struct SalesforceScanBindData : public FunctionData {
    SalesforceConfig config;
    SalesforceTokenSet token; // obtained at ATTACH, reused here
    string object;
    vector<SalesforceField> fields; // queryable fields, in column order
    vector<string> column_names;
    vector<LogicalType> column_types;
    // Pushed-down SOQL WHERE (set by pushdown_complex_filter; #9). Empty => none.
    string pushed_where;

    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other) const override;
};

// The catalog scan TableFunction. Used only via the catalog (bind_data is
// pre-built by GetScanFunction); calling it standalone is unsupported.
TableFunction GetSalesforceScanFunction();

} // namespace duckdb

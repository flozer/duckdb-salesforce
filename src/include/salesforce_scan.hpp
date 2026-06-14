#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_diag.hpp" // DiagExplainItem (explain capture)

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
    // Query-cost diagnostics (#v0.4 §4): how many conjunctive filters were
    // pushed to SOQL vs left residual for DuckDB. Set by pushdown_complex_filter.
    int64_t pushed_filter_count = 0;
    int64_t residual_filter_count = 0;
    // Owning ATTACH alias + per-filter explain capture (#v1.6 query_explain).
    // Diagnostic-only: written by pushdown, never read by the scan path.
    string catalog_alias;
    vector<DiagExplainItem> explain_filters;
    // The pushdown hook may fire more than once with a re-presented residual
    // filter; capture the explain items only on the first non-empty call.
    bool explain_captured = false;

    unique_ptr<FunctionData> Copy() const override;
    bool Equals(const FunctionData &other) const override;
};

// The catalog scan TableFunction. Used only via the catalog (bind_data is
// pre-built by GetScanFunction); calling it standalone is unsupported.
TableFunction GetSalesforceScanFunction();

} // namespace duckdb

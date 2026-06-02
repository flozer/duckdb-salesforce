#pragma once

#include "duckdb.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/table_function.hpp"
#include "salesforce_describe.hpp"

namespace duckdb {

class Expression;

// Format a DuckDB Value as a SOQL literal (string-quoted+escaped, numbers/bools
// bare, date/datetime/time in Salesforce form). Throws on an unsupported type
// — the caller treats that filter as residual.
string SoqlLiteral(const Value &value);

// SELECT <fields> FROM <object> [WHERE <where>] [LIMIT n]. where_clause empty
// => no WHERE; limit invalid => no LIMIT.
string BuildSelectSoql(const string &object, const vector<string> &select_fields,
                       const string &where_clause, optional_idx limit);

// Best-effort predicate pushdown for a table function's pushdown_complex_filter
// hook. `filters` is the conjunctive list on the scan; the safe, conservative
// subset (=, <>, <, <=, >, >=, IS [NOT] NULL on filterable fields, with
// string/number/bool/temporal constants) is translated into `out_where` and
// REMOVED from `filters`; everything else is left for DuckDB to apply
// residually. Applies the WHERE length guard (over-long => nothing pushed).
// Never surfaces a secret or a large dump on error — untranslatable filters are
// simply left residual.
// `projection_to_field` maps a filter's column-ref index (projection-relative)
// to the index of the field in `fields`.
void PushdownToSoql(const vector<SalesforceField> &fields,
                    const vector<idx_t> &projection_to_field, string &out_where,
                    vector<unique_ptr<Expression>> &filters);

// Diagnostic: record / read the most recent SOQL a scan generated. Used by the
// salesforce_last_soql() table function so tests can assert the pushdown.
void SetLastSoql(const string &soql);
TableFunction GetSalesforceLastSoqlFunction();

// DEBUG / TEST ONLY. Number of query pages the most recent scan fetched, so
// tests can prove the lazy/streaming scan (#11) stopped before later pages.
// Not a stable/public API.
void SetLastScanPages(idx_t pages);
TableFunction GetSalesforceLastScanPagesFunction();

} // namespace duckdb

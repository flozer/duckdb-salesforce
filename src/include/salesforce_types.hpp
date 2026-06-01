#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Map a Salesforce field "type" (sObject describe) to a DuckDB LogicalType.
// precision/scale are used for currency/percent decimals. Unknown types map
// conservatively to VARCHAR and set *unknown=true so the caller can emit a
// secret-free warning. Case-insensitive on the type name.
LogicalType MapSalesforceType(const string &sf_type, int64_t precision, int64_t scale,
                              bool *unknown);

} // namespace duckdb

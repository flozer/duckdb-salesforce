#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Map a Salesforce field "type" (sObject describe) to a DuckDB LogicalType.
// precision/scale are used for currency/percent decimals. Unknown types map
// conservatively to VARCHAR and set *unknown=true so the caller can emit a
// secret-free warning. Case-insensitive on the type name.
LogicalType MapSalesforceType(const string &sf_type, int64_t precision, int64_t scale,
                              bool *unknown);

// Map a Tooling API FieldDefinition.DataType DISPLAY string (e.g. "Text(255)",
// "Number(18,0)", "Checkbox", "Date/Time", "Lookup(Account)") to a DuckDB
// LogicalType (#v0.6 §6). Coarser than the REST describe `type`. Sets *ok=false
// for ambiguous/unmapped types (Formula, Roll-Up Summary, unknown) so the caller
// falls back to the authoritative REST describe for that object.
LogicalType MapToolingDataType(const string &data_type, bool *ok);

} // namespace duckdb

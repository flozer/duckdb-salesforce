#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

struct SalesforceField;

// Decode one Salesforce JSON record's value for `field` into vec[row] using the
// field's mapped DuckDB type. A JSON null or an absent key sets the row null.
// On an undecodable value it throws a clear error naming the field and types —
// never the value and never the whole record.
//
// No HTTP, no network: operates only on already-fetched record JSON (#6).
void AppendJsonValue(Vector &vec, idx_t row, const SalesforceField &field,
                     const string &record_json);

// Decode a non-null string cell (e.g. a Bulk CSV cell) into vec[row] using the
// field's mapped type — the shared cast core (date/time normalisation, base64,
// DuckDB cast). Throws a clear, field-named, value-free error on failure. The
// caller handles null (CSV empty cell / JSON null) before calling this.
void AppendTypedCell(Vector &vec, idx_t row, const SalesforceField &field, const string &cell);

// salesforce_decode(fields_json, records_json) -> typed columns.
// fields_json: '{"fields":[{"name","type","precision","scale",...}]}'
// records_json: '[{...},{...}]'
// Test surface that drives AppendJsonValue with inline data (no scanner, #8).
TableFunction GetSalesforceDecodeFunction();

} // namespace duckdb

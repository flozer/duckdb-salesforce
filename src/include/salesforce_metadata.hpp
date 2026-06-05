#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Narrow, read-only metadata enrichment (#v1.3 §14) over the REST describe of an
// already-ATTACHed catalog (cached per ATTACH). NOT the Metadata API: no deploy,
// retrieve, or CRUD.

// salesforce_picklist_values(catalog, object, field) -> one row per picklist
// value of the field: value, label, active, is_default. The full field catalog
// (active + inactive), not a record-type-filtered subset.
TableFunction GetSalesforcePicklistValuesFunction();

// salesforce_record_types(catalog, object) -> one row per record type:
// developer_name, label, record_type_id, active, is_default.
TableFunction GetSalesforceRecordTypesFunction();

} // namespace duckdb

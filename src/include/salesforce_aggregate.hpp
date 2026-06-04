#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// salesforce_aggregate(catalog, object, aggregates [, filter]) — explicit,
// opt-in server-side SOQL aggregates (#v1.0). Runs
//   SELECT <aggregates> FROM <object> [WHERE <filter>]
// over an already-ATTACHed catalog's authenticated session and returns ONE row,
// one VARCHAR column per aggregate term (named by its alias, else expr0/1/...).
// This is NOT transparent pushdown — the user chooses it explicitly. No
// optimizer, no plan rewrite. GROUP BY is out of scope for this cut.
TableFunction GetSalesforceAggregateFunction();

} // namespace duckdb

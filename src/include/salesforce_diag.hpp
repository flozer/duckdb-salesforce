#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Query-cost diagnostics (#v0.4 §4). A single consolidated, LAST-SCAN (not
// historical) snapshot of what the most recent catalog scan cost and why:
// SOQL, transport, projection ratio, pushed vs residual filter counts, pages,
// rows delivered to DuckDB, and (when consulted) the quota decision. Surfaced
// by salesforce_query_cost(). Best-effort + last-wins; scans are single-threaded
// (MaxThreads=1) so concurrent scans in one process would overwrite each other.

// Record the per-scan facts known at InitGlobal (resets pages/rows/quota for the
// new scan). `est_rows` < 0 and `pages` < 0 are emitted as NULL.
void DiagRecordScan(const string &object, const string &soql, const string &transport,
                    int64_t est_rows, const string &transport_reason, int64_t projected_fields,
                    int64_t total_fields, int64_t pushed_filters, int64_t residual_filters,
                    const string &where_pushed, bool bulk, int64_t pages_init, bool count_pushdown);

// REST page counter mirror (kept in sync with salesforce_last_scan_pages()).
void DiagSetPages(int64_t pages);

// Rows DELIVERED to DuckDB (output cardinality), accumulated across chunks.
void DiagAddRowsEmitted(int64_t rows);

// Quota decision mirror (only when the governor actually consulted /limits).
// `remaining` < 0 -> NULL.
void DiagSetQuota(int64_t remaining, bool allowed);

TableFunction GetSalesforceQueryCostFunction();

} // namespace duckdb

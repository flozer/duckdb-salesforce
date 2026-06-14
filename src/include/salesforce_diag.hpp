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
                    const string &where_pushed, bool bulk, int64_t pages_init, bool count_pushdown,
                    const string &query_mode);

// --- scan EXPLAIN capture (#v1.6 metadata-driven scan diagnostics) -----------
// Write-only, diagnostic-only mirror of what the last scan ALREADY classified:
// one item per projected field and per conjunctive filter. The scan path never
// reads these back, so they cannot affect execution. salesforce_query_explain()
// annotates them via the shared Metadata Engine.
struct DiagExplainItem {
    string role;       // "projection" | "filter"
    string field;      // resolved field name ("" when field_known=false)
    bool field_known = false; // false => no single field (complex expr) -> NULL
    bool pushed = false;   // emitted into SOQL (SELECT for projection / WHERE for filter)
    bool residual = false; // filter re-applied by DuckDB (false for projection)
};

// Snapshot of the last scan's explain capture (object + catalog alias + items).
struct DiagExplainSnapshot {
    string object;
    string catalog_alias; // empty => unknown -> annotation degrades
    vector<DiagExplainItem> items;
};

// Record the explain capture for the current scan. Called at InitGlobal AFTER
// DiagRecordScan (which resets the snapshot), so it is not wiped.
void DiagSetExplain(const string &catalog_alias, vector<DiagExplainItem> items);

// Read the last scan's explain snapshot (object filled by DiagRecordScan).
DiagExplainSnapshot DiagGetExplain();

// REST page counter mirror (kept in sync with salesforce_last_scan_pages()).
void DiagSetPages(int64_t pages);

// Bulk PK-chunk count (#v0.7 §9): how many Id-range chunks the Bulk scan ran
// (1 = no chunking). Surfaced as salesforce_query_cost().bulk_chunks.
void DiagSetBulkChunks(int64_t chunks);

// Bulk job status-poll count (ROADMAP §15): how many times BulkStartJob polled
// the job before it completed (or timed out). Surfaced as
// salesforce_query_cost().bulk_polls; NULL for non-Bulk scans.
void DiagSetBulkPolls(int64_t polls);

// Rows DELIVERED to DuckDB (output cardinality), accumulated across chunks.
void DiagAddRowsEmitted(int64_t rows);

// Quota decision mirror (only when the governor actually consulted /limits).
// `remaining` < 0 -> NULL.
void DiagSetQuota(int64_t remaining, bool allowed);

TableFunction GetSalesforceQueryCostFunction();

} // namespace duckdb

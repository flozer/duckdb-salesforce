#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Report Bridge (ROADMAP §16).
//
// Phase A ships only the analytics HTTP foundation. salesforce_report_fetch_raw
// is a TEST/foundation harness that triggers the synchronous report run /
// describe path so offline tests can prove endpoint routing, GET method, intact
// response delivery, basic JSON parse, and clear errors — without live
// Salesforce. The user-facing salesforce_reports() / salesforce_report() /
// salesforce_report_soql() functions arrive in Phases B-D.
TableFunction GetSalesforceReportFetchRawFunction();

} // namespace duckdb

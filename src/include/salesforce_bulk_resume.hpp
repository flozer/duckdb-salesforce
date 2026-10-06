#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Re-stream the results of a Bulk API 2.0 query job by id (consumer feedback
// #4): when a load dies mid-streaming of a job that already completed
// server-side, this re-opens the job's result pages without re-running the
// query. Columns come from the job's CSV header, emitted as VARCHAR (Bulk CSV
// carries no type metadata); rows stream page-by-page following the
// Sforce-Locator.
TableFunction GetSalesforceBulkResumeFunction();

} // namespace duckdb

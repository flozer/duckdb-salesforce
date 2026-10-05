#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Delete-sync primitive (consumer feedback #1): surfaces the rows deleted in
// the org within a time window, so a watermark-based incremental load can
// pair its upsert with a delete sweep. Two sources, chosen by window width:
//  - window <= 15 minutes: Replication API getDeleted() (exact deletedDate);
//  - wider window: queryAll scan `WHERE IsDeleted = true AND SystemModstamp >=
//    since` (recycle-bin retention ~15 days; deletion time NOT available —
//    emitted as NULL, source='queryAll').
TableFunction GetSalesforceDeletedIdsFunction();

} // namespace duckdb

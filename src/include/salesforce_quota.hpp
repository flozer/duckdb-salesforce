#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class SalesforceSession;

// Quota governor (#v0.4). Gate a Bulk query-job START on the org's REST API
// allocation. Reads the sf_quota_* settings, consults /limits (cached per
// instance_url, in-memory only, TTL-bounded), records the decision for
// salesforce_last_quota(), and THROWS a clear, secret-free error when enforcing
// and the remaining quota is at/below the reserve. Bulk-only: small/REST scans
// are never preflight-gated. Policy:
//   - sf_quota_enabled=false  -> skip /limits entirely, never block.
//   - sf_quota_enforce=false  -> consult /limits, compute, but never block (warn).
//   - /limits unavailable     -> fail-open (allow) unless sf_quota_fail_open=false.
void QuotaGuardBulkStart(ClientContext &context, SalesforceSession &session,
                         const string &instance_url);

// DEBUG / diagnostic. The last governor decision:
// (limit_name, max, remaining, threshold, allowed, reason).
TableFunction GetSalesforceLastQuotaFunction();

} // namespace duckdb

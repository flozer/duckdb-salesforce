#pragma once

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

// Transparent aggregate pushdown (P1.2a/P1.2b/P1.3; roadmap P1; spec
// docs/superpowers/specs/2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md).
// Post-optimizer hook: rewrites Aggregate(no groups: COUNT(col) |
// COUNT(DISTINCT col) | MIN(col) | MAX(col), ...) directly over a
// salesforce_scan Get into one server-side aggregate SOQL query emitting a
// single row, replacing the Aggregate with a binding-preserving Projection.
// Guarded: any unsupported shape (GROUP BY, COUNT(*), expressions, FILTER/
// ORDER BY, non-sortable or string-typed MIN/MAX fields, relationships,
// blobs, residual filters, kill-switch off) leaves the plan untouched --
// DuckDB then aggregates locally over the normal row scan, which is always
// correct.
void SalesforceAggregatePushdownOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb

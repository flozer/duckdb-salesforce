#pragma once

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

// Transparent aggregate pushdown (P1.2a; roadmap P1; spec
// docs/superpowers/specs/2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md).
// Post-optimizer hook: rewrites Aggregate(no groups: COUNT(col), ...) directly
// over a salesforce_scan Get into one server-side COUNT query emitting a
// single row, replacing the Aggregate with a binding-preserving Projection.
// Guarded: any unsupported shape (GROUP BY, COUNT(*), DISTINCT, expressions,
// relationships, blobs, residual filters, kill-switch off) leaves the plan
// untouched -- DuckDB then aggregates locally over the normal row scan, which
// is always correct.
void SalesforceAggregatePushdownOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

} // namespace duckdb

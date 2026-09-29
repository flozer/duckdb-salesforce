// Transparent aggregate pushdown (P1.2a/P1.2b/P1.3; roadmap P1, spec
// docs/superpowers/specs/2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md).
//
// A post-optimizer pass (OptimizerExtension::optimize_function, i.e. it runs
// AFTER every built-in pass and owns the final plan shape) that finds
//
//     Aggregate(no groups: COUNT(col)|COUNT(DISTINCT col)|MIN(col)|MAX(col), ...)
//       -> Get(salesforce_scan)
//
// and turns it into a scan that runs ONE server-side aggregate query
// (SELECT <fn>(col) a0, ... FROM obj WHERE <pushed_where>) emitting a single
// result row, with the Aggregate node replaced by a binding-preserving
// Projection.
//
// Safety contract: the rewrite happens ONLY when every guard holds; otherwise
// the plan is left untouched and DuckDB aggregates locally over the normal
// row scan (today's always-correct fallback):
//   - no GROUP BY / grouping sets;
//   - every aggregate is a single-column call of a supported function
//     (COUNT, COUNT_DISTINCT, MIN, MAX) -- no FILTER, no ORDER BY, no
//     expression children (COUNT(*) keeps the existing zero-column scan
//     path); SUM/AVG stay deferred (P1.4, decimal evidence);
//   - the referenced field is top-level, non-relationship and non-blob;
//     MIN/MAX additionally require a sortable field of a numeric, temporal
//     or boolean DuckDB type (strings keep local aggregation: SOQL and
//     DuckDB collation semantics are not proven equivalent);
//   - ZERO residual filters (bind_data.residual_filter_count == 0): a
//     residually-filtered row stream must not be replaced by a server
//     aggregate;
//   - the sf_aggregate_pushdown kill-switch is on (checked here; the scan
//     side has no choice once the plan is rewritten, by design).

#include "salesforce_agg_optimizer.hpp"
#include "salesforce_scan.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

namespace {

// MIN/MAX pushdown type gate (P1.3): numeric, temporal and boolean DuckDB
// types only. Strings stay local (SOQL vs DuckDB collation semantics are not
// proven equivalent); BLOB/STRUCT are unreachable here but stay rejected.
bool AggPushableMinMaxType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return true;
	default:
		return false;
	}
}

// Validate the Aggregate->Get pattern and, on success, annotate the scan's
// bind data with the pushed-aggregate spec and rebuild the Get's output schema
// (one BIGINT column per aggregate term). Returns the Projection expressions
// mapping the new Get outputs to the OLD aggregate bindings via out_colrefs.
bool TryRewriteCountAggregate(ClientContext &context, LogicalAggregate &agg,
                              vector<unique_ptr<Expression>> &out_colrefs) {
	if (!agg.groups.empty() || !agg.grouping_sets.empty() || !agg.grouping_functions.empty()) {
		return false; // transparent GROUP BY stays deferred (local is correct)
	}
	if (agg.expressions.empty() || agg.children.size() != 1) {
		return false;
	}
	// Direct child only: any intervening operator (e.g. a residual LogicalFilter)
	// both fails this check and must block the rewrite (its predicate would
	// otherwise be applied AFTER the pre-aggregated single row).
	if (agg.children[0]->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = agg.children[0]->Cast<LogicalGet>();
	if (get.function.name != "salesforce_scan" || !get.bind_data) {
		return false;
	}
	SalesforceScanBindData *bind = nullptr;
	try {
		bind = &get.bind_data->Cast<SalesforceScanBindData>();
	} catch (...) {
		return false; // not our bind data
	}
	if (bind->residual_filter_count != 0 || !bind->pushed_aggregates.empty()) {
		return false;
	}
	if (!get.table_filters.filters.empty()) {
		return false; // standard TableFilters would reference the old schema
	}
	// Kill-switch (default on when unset).
	Value sw;
	if (context.TryGetCurrentSetting("sf_aggregate_pushdown", sw)) {
		if (sw.type().id() == LogicalTypeId::BOOLEAN && !sw.GetValue<bool>()) {
			return false;
		}
	}

	vector<SalesforceScanBindData::SalesforcePushedAggregate> pushed;
	// Aggregate expressions live in the inherited LogicalOperator::expressions
	// across the supported matrix (v1.5.4-v1.5.6; there is no dedicated
	// `aggregates` member).
	for (auto &expr : agg.expressions) {
		if (expr->type != ExpressionType::BOUND_AGGREGATE) {
			return false;
		}
		auto &bag = expr->Cast<BoundAggregateExpression>();
		if (bag.filter || bag.order_bys) {
			return false; // FILTER / ORDER BY inside the aggregate stays local
		}
		// Supported functions (SOQL names): COUNT, COUNT_DISTINCT (P1.2b),
		// MIN/MAX (P1.3). SUM/AVG stay deferred (P1.4, decimal evidence);
		// COUNT(*) has no children: the zero-column scan path owns it.
		const auto fname = StringUtil::Lower(bag.function.name);
		const bool is_count = fname == "count";
		const bool is_minmax = fname == "min" || fname == "max";
		if (!is_count && !is_minmax) {
			return false;
		}
		if (bag.IsDistinct() && !is_count) {
			return false; // DISTINCT is only defined for COUNT in SOQL
		}
		if (bag.children.size() != 1) {
			return false;
		}
		if (bag.children[0]->type != ExpressionType::BOUND_COLUMN_REF) {
			return false; // aggregate over an expression stays local
		}
		auto &ref = bag.children[0]->Cast<BoundColumnRefExpression>();
		if (ref.binding.table_index != get.table_index) {
			return false;
		}
		// Post-pruning binding space: the Get's column_ids (ColumnIndex list) is
		// BOTH the binding space (0..N-1) AND the map back to the original field
		// index in bind->fields. A binding column_index must be resolved through
		// it -- never used directly as a fields[] index.
		auto binding_idx = ref.binding.column_index;
		if (binding_idx >= get.GetColumnIds().size()) {
			return false;
		}
		auto original_idx = get.GetColumnIds()[binding_idx].GetPrimaryIndex();
		if (original_idx >= bind->fields.size()) {
			return false; // virtual/rowid column
		}
		const auto &field = bind->fields[original_idx];
		if (field.is_relationship || !field.children.empty() || field.name.find('.') != string::npos) {
			return false; // relationship STRUCTs stay out of this pass
		}
		if (StringUtil::Lower(field.sf_type) == "base64") {
			return false; // blob fields are not server-aggregable
		}
		if (is_minmax) {
			if (!field.sortable) {
				return false; // SOQL MIN/MAX need ORDER BY semantics: sortable
			}
			if (!AggPushableMinMaxType(field.duckdb_type)) {
				return false; // VARCHAR (collation), BLOB, STRUCT: stay local
			}
		}
		SalesforceScanBindData::SalesforcePushedAggregate pa;
		if (is_count) {
			pa.func = bag.IsDistinct() ? "COUNT_DISTINCT" : "COUNT";
			// COUNT / COUNT(DISTINCT) return BIGINT in both engines.
			pa.emit.duckdb_type = LogicalType::BIGINT;
		} else {
			pa.func = StringUtil::Upper(fname);
			pa.emit.duckdb_type = field.duckdb_type;
		}
		pa.field = field.name;
		pa.out_name = expr->GetName();
		// Decode surface: the SOQL result key is the alias (a0, a1, ...); the
		// describe field's type metadata drives AppendJsonValue's conversion.
		pa.emit.name = "a" + std::to_string(pushed.size());
		pa.emit.sf_type = field.sf_type;
		pushed.push_back(std::move(pa));
	}

	// Commit: annotate the scan, rebuild the Get schema, prepare the Projection
	// expressions. Reusing the aggregate's table index keeps every PARENT
	// ColumnBinding (aggregate_index, i) valid without touching operators above.
	auto get_index = get.table_index;
	vector<LogicalType> types;
	vector<string> names;
	for (idx_t i = 0; i < pushed.size(); i++) {
		types.push_back(pushed[i].emit.duckdb_type);
		names.push_back(pushed[i].out_name);
		out_colrefs.push_back(
		    make_uniq<BoundColumnRefExpression>(pushed[i].emit.duckdb_type, ColumnBinding(get_index, i)));
	}
	bind->pushed_aggregates = std::move(pushed);
	get.returned_types = std::move(types);
	get.names = std::move(names);
	// The Get's column_ids is the binding space AND the map into the original
	// schema. Rebuild it as the identity over the NEW (aggregate) schema so the
	// physical scan emits exactly the k aggregate columns in order; the stale
	// entries referenced the pruned row schema and would resolve out of bounds.
	get.SetColumnIds(vector<ColumnIndex>());
	auto &col_ids = get.GetMutableColumnIds();
	for (idx_t i = 0; i < out_colrefs.size(); i++) {
		col_ids.push_back(ColumnIndex(i));
	}
	get.projection_ids.clear(); // struct-extract map referenced the old schema
	return true;
}

void Walk(ClientContext &context, unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = op->Cast<LogicalAggregate>();
		vector<unique_ptr<Expression>> colrefs;
		if (TryRewriteCountAggregate(context, agg, colrefs)) {
			auto proj = make_uniq<LogicalProjection>(agg.aggregate_index, std::move(colrefs));
			proj->children.push_back(std::move(agg.children[0]));
			op = std::move(proj);
			return; // subtree moved; nothing left to walk here
		}
	}
	for (auto &child : op->children) {
		Walk(context, child);
	}
}

} // namespace

void SalesforceAggregatePushdownOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Walk(input.context, plan);
}

} // namespace duckdb

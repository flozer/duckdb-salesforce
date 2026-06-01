// Catalog-driven sObject scan (issue #8).
//
// InitGlobal runs the SOQL query for the bound object (SELECT <all queryable
// fields> FROM <object>) via SalesforceSession (auth token reused from ATTACH,
// pagination + 401 refresh from #6), collecting raw JSON records. The scan
// function decodes each record into the output chunk with AppendJsonValue (#7).
// No pushdown — the full field list is always selected (pushdown is #9).

#include "salesforce_scan.hpp"
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_value.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

unique_ptr<FunctionData> SalesforceScanBindData::Copy() const {
    auto r = make_uniq<SalesforceScanBindData>();
    r->config = config;
    r->token = token;
    r->object = object;
    r->fields = fields;
    r->column_names = column_names;
    r->column_types = column_types;
    r->pushed_where = pushed_where;
    return std::move(r);
}

bool SalesforceScanBindData::Equals(const FunctionData &other_p) const {
    auto &other = other_p.Cast<SalesforceScanBindData>();
    return object == other.object && column_names == other.column_names;
}

namespace {

struct ScanGlobalState : public GlobalTableFunctionState {
    vector<string> records;
    idx_t cursor = 0;
    // DuckDB-level projection: which source field each output column maps to.
    vector<column_t> column_ids;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> ScanBind(ClientContext &, TableFunctionBindInput &,
                                         vector<LogicalType> &, vector<string> &) {
    throw InternalException(
        "salesforce_scan is catalog-internal and cannot be called directly");
}

static unique_ptr<GlobalTableFunctionState> ScanInitGlobal(ClientContext &context,
                                                           TableFunctionInitInput &input) {
    auto &bind = input.bind_data->Cast<SalesforceScanBindData>();
    auto gstate = make_uniq<ScanGlobalState>();
    gstate->column_ids = input.column_ids; // DuckDB-level projection

    // Projection pushdown: SELECT only the referenced fields. Fall back to the
    // first field when nothing is projected (e.g. COUNT(*)).
    vector<string> select_fields;
    for (auto col : input.column_ids) {
        if (col < bind.fields.size()) {
            select_fields.push_back(bind.fields[col].name);
        }
    }
    if (select_fields.empty() && !bind.fields.empty()) {
        select_fields.push_back(bind.fields[0].name);
    }

    // Predicate pushdown: the WHERE was translated in pushdown_complex_filter
    // (#9); untranslated predicates remain in the plan and DuckDB applies them
    // residually, so results are always correct.
    //
    // LIMIT pushdown is not wired: this DuckDB build does not expose the query
    // LIMIT to a table function, so LIMIT is applied residually by DuckDB.
    string soql = BuildSelectSoql(bind.object, select_fields, bind.pushed_where, optional_idx());
    SetLastSoql(soql);

    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(bind.config, *client);
    session.SetToken(bind.token); // reuse ATTACH token (refreshes on 401)
    gstate->records = session.Query(soql).records;
    return std::move(gstate);
}

static void ScanFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<SalesforceScanBindData>();
    auto &gstate = data.global_state->Cast<ScanGlobalState>();

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gstate.cursor < gstate.records.size()) {
        const string &record = gstate.records[gstate.cursor];
        // One output vector per projected column; a column_id beyond the field
        // list is a virtual column (e.g. the COUNT(*) row marker) -> set NULL.
        for (idx_t j = 0; j < gstate.column_ids.size(); j++) {
            column_t col = gstate.column_ids[j];
            if (col >= bind.fields.size()) {
                FlatVector::SetNull(output.data[j], row, true);
            } else {
                AppendJsonValue(output.data[j], row, bind.fields[col], record);
            }
        }
        gstate.cursor++;
        row++;
    }
    output.SetChildCardinality(row);
}

// Predicate pushdown: translate the safe subset of the conjunctive filter list
// into a SOQL WHERE on the bind data; DuckDB keeps the rest as a residual
// Filter operator. (#9)
static void ScanPushdownComplexFilter(ClientContext &, LogicalGet &get, FunctionData *bind_data,
                                      vector<unique_ptr<Expression>> &filters) {
    if (!bind_data) {
        return;
    }
    auto &bind = bind_data->Cast<SalesforceScanBindData>();
    // Map the GET's projection-relative column refs to field indices.
    vector<idx_t> projection_to_field;
    for (auto &ci : get.GetColumnIds()) {
        projection_to_field.push_back(ci.GetPrimaryIndex());
    }
    PushdownToSoql(bind.fields, projection_to_field, bind.pushed_where, filters);
}

} // namespace

TableFunction GetSalesforceScanFunction() {
    TableFunction fn("salesforce_scan", {}, ScanFunction, ScanBind, ScanInitGlobal);
    fn.projection_pushdown = true; // DuckDB-level column projection
    fn.pushdown_complex_filter = ScanPushdownComplexFilter; // SOQL WHERE, residual-safe
    return fn;
}

} // namespace duckdb

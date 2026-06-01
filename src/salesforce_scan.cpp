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
#include "salesforce_value.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

unique_ptr<FunctionData> SalesforceScanBindData::Copy() const {
    auto r = make_uniq<SalesforceScanBindData>();
    r->config = config;
    r->token = token;
    r->object = object;
    r->fields = fields;
    r->column_names = column_names;
    r->column_types = column_types;
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

    // SELECT <all queryable fields> FROM <object>.
    string field_list;
    for (idx_t i = 0; i < bind.fields.size(); i++) {
        if (i > 0) {
            field_list += ", ";
        }
        field_list += bind.fields[i].name;
    }
    string soql = "SELECT " + field_list + " FROM " + bind.object;

    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(bind.config, *client);
    session.SetToken(bind.token); // reuse ATTACH token (refreshes on 401)
    gstate->records = session.Query(soql).records;
    gstate->column_ids = input.column_ids; // DuckDB-level projection
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

} // namespace

TableFunction GetSalesforceScanFunction() {
    TableFunction fn("salesforce_scan", {}, ScanFunction, ScanBind, ScanInitGlobal);
    fn.projection_pushdown = true; // DuckDB-level column projection (not SOQL)
    return fn;
}

} // namespace duckdb

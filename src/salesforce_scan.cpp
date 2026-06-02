// Catalog-driven sObject scan (issues #8/#9/#11).
//
// InitGlobal builds the SOQL (projection #9 + WHERE #9) and keeps the session +
// pagination state alive, but fetches NOTHING yet. ScanFunction streams pages
// LAZILY (#11): it fetches the next page (queryMore) only when the current page
// is exhausted and the chunk still needs rows. So a query with a small LIMIT
// makes DuckDB stop pulling after the first chunk and later pages are never
// fetched. Records decode with AppendJsonValue (#7); 401 refresh + loop guards
// preserved. LIMIT is NOT pushed to SOQL — it is applied residually by DuckDB.

#include "salesforce_scan.hpp"
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_value.hpp"

#include <unordered_set>

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

// Bounds a misbehaving server cursor (same ceiling as the eager Query path).
static constexpr idx_t kScanMaxPages = 1000000;

struct ScanGlobalState : public GlobalTableFunctionState {
    // Declared before `session` so the client outlives the session that
    // references it (destruction is reverse declaration order).
    unique_ptr<SalesforceHttpClient> client;
    unique_ptr<SalesforceSession> session;

    string next_path;        // initial query path, then each nextRecordsUrl
    bool done = false;       // no more pages after the current one is drained
    vector<string> page;     // current page's raw records
    idx_t cursor = 0;        // index into `page`
    idx_t pages_fetched = 0;
    std::unordered_set<string> seen; // queryMore cursors seen (loop guard)

    // Bulk path (sf_force_transport='bulk', #v0.3): eager CSV result.
    bool bulk = false;
    SalesforceBulkResult bulk_result;
    vector<int64_t> field_to_csv; // field index -> CSV column index (-1 if absent)
    idx_t bulk_cursor = 0;

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

    // Transport: 'rest' (default, lazy) or 'bulk' (Bulk API 2.0). Same optimized
    // SOQL either way, so projection/predicate pushdown applies to both.
    string transport = "rest";
    Value tv;
    if (context.TryGetCurrentSetting("sf_force_transport", tv) && !tv.IsNull()) {
        transport = StringUtil::Lower(tv.ToString());
    }
    if (transport != "rest" && transport != "bulk") {
        throw BinderException("sf_force_transport must be 'rest' or 'bulk' (got '%s').", transport);
    }

    gstate->client = BuildHttpClientForContext(context);
    gstate->session = make_uniq<SalesforceSession>(bind.config, *gstate->client);
    gstate->session->SetToken(bind.token); // reuse ATTACH token (refreshes on 401)
    SetLastScanPages(0);

    if (transport == "bulk") {
        gstate->bulk = true;
        gstate->bulk_result = gstate->session->BulkQuery(soql);
        // Map each field to its CSV column index (header may reorder).
        gstate->field_to_csv.assign(bind.fields.size(), -1);
        for (idx_t f = 0; f < bind.fields.size(); f++) {
            for (idx_t c = 0; c < gstate->bulk_result.columns.size(); c++) {
                if (StringUtil::CIEquals(bind.fields[f].name, gstate->bulk_result.columns[c])) {
                    gstate->field_to_csv[f] = static_cast<int64_t>(c);
                    break;
                }
            }
        }
    } else {
        // REST: build the initial page path, but fetch NOTHING yet (lazy, #11).
        gstate->next_path = gstate->session->QueryPath(soql);
    }
    return std::move(gstate);
}

// Fetch the next page into gstate. Returns false when there are no more rows.
// Skips empty-but-not-done pages (advancing the cursor) and guards against a
// repeated nextRecordsUrl or runaway page count.
static bool ScanAdvancePage(ScanGlobalState &g) {
    while (true) {
        if (g.done) {
            return false;
        }
        SalesforceQueryPage pg = g.session->FetchPage(g.next_path);
        g.pages_fetched++;
        SetLastScanPages(g.pages_fetched);
        g.page = std::move(pg.records);
        g.cursor = 0;

        bool last = pg.done || pg.next_path.empty();
        if (!last) {
            if (g.pages_fetched >= kScanMaxPages) {
                throw IOException("salesforce scan: exceeded the maximum page count (%llu).",
                                  static_cast<unsigned long long>(kScanMaxPages));
            }
            if (!g.seen.insert(pg.next_path).second) {
                throw IOException(
                    "salesforce scan: pagination loop detected (nextRecordsUrl repeated).");
            }
            g.next_path = pg.next_path;
        } else {
            g.done = true;
        }

        if (!g.page.empty()) {
            return true;
        }
        if (g.done) {
            return false; // empty final page
        }
        // empty page but more to come -> loop to fetch the next one
    }
}

static void ScanFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<SalesforceScanBindData>();
    auto &gstate = data.global_state->Cast<ScanGlobalState>();

    // Bulk path: emit decoded CSV rows (already downloaded in InitGlobal).
    if (gstate.bulk) {
        idx_t row = 0;
        while (row < STANDARD_VECTOR_SIZE && gstate.bulk_cursor < gstate.bulk_result.rows.size()) {
            const auto &cells = gstate.bulk_result.rows[gstate.bulk_cursor];
            for (idx_t j = 0; j < gstate.column_ids.size(); j++) {
                column_t col = gstate.column_ids[j];
                int64_t ci = (col < bind.fields.size()) ? gstate.field_to_csv[col] : -1;
                if (ci < 0 || static_cast<idx_t>(ci) >= cells.size() || cells[ci].empty()) {
                    FlatVector::SetNull(output.data[j], row, true); // missing/virtual/empty -> NULL
                } else {
                    AppendTypedCell(output.data[j], row, bind.fields[col], cells[ci]);
                }
            }
            gstate.bulk_cursor++;
            row++;
        }
        output.SetCardinality(row);
        return;
    }

    // Fetch the next page only when the current one is fully drained. Emit at
    // PAGE granularity (one page per call, capped at the chunk size): this lets
    // DuckDB's LIMIT operator stop pulling between calls, so a small LIMIT never
    // triggers the next-page fetch.
    if (gstate.cursor >= gstate.page.size()) {
        if (!ScanAdvancePage(gstate)) {
            output.SetCardinality(0);
            return;
        }
    }

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gstate.cursor < gstate.page.size()) {
        const string &record = gstate.page[gstate.cursor];
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
    output.SetCardinality(row);
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

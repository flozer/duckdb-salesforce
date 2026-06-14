// Query-cost diagnostics (#v0.4 §4). One consolidated LAST-SCAN snapshot +
// the salesforce_query_cost() table function. Pure in-memory diagnostic; never
// holds a secret (the SOQL is the user's own query text, no bearer/body).

#include "salesforce_diag.hpp"

#include "duckdb/common/string_util.hpp"

#include <mutex>

namespace duckdb {

namespace {

struct ScanCost {
    string object;
    string soql;
    string transport;
    int64_t est_rows = -1; // -1 -> NULL
    string transport_reason;
    int64_t projected_fields = 0;
    int64_t total_fields = 0;
    int64_t pushed_filters = 0;
    int64_t residual_filters = 0;
    string where_pushed;
    int64_t pages_fetched = -1; // -1 -> NULL (e.g. Bulk)
    int64_t rows_emitted = 0;
    bool bulk = false;
    bool count_pushdown = false;
    int64_t bulk_chunks = 1; // PK-chunk count (#v0.7 §9); 1 = no chunking
    int64_t bulk_polls = -1; // ROADMAP §15: Bulk status polls; -1 -> NULL (not Bulk)
    string query_mode = "query"; // #v0.9 §1: query | queryAll
    bool quota_consulted = false; // false -> quota_* NULL
    int64_t quota_remaining = -1;
    bool quota_allowed = false;
    string catalog_alias;                 // #v1.6 explain: owning ATTACH alias
    vector<DiagExplainItem> explain_items; // #v1.6 explain: projection + filters
};

std::mutex g_lock;
ScanCost g_cost;

// Short, actionable selectivity hints (not an essay). Joined with "; ".
string BuildGuidance(const ScanCost &c) {
    vector<string> hints;
    // The §15 distinction: is Salesforce filtering server-side, or is DuckDB
    // filtering locally after dragging a full remote scan across the wire?
    if (!c.where_pushed.empty()) {
        hints.push_back("Salesforce is filtering server-side (predicate pushed to SOQL)");
    } else {
        hints.push_back("no predicate pushed to SOQL — full-object scan; add a filterable WHERE");
    }
    if (c.residual_filters > 0) {
        hints.push_back(StringUtil::Format(
            "DuckDB is filtering after a full remote scan: %lld filter(s) applied "
            "residually (not server-filterable) — over-fetch",
            (long long)c.residual_filters));
    }
    if (c.bulk && c.where_pushed.empty()) {
        hints.push_back("Bulk full-object read with no pushed predicate — for a large "
                        "backfill, split by CreatedDate/SystemModstamp windows, or set "
                        "sf_bulk_require_predicate to enforce it");
    }
    if (c.total_fields > 0 && c.projected_fields >= c.total_fields) {
        hints.push_back("all fields projected — SELECT fewer columns to cut egress");
    }
    if (c.bulk) {
        hints.push_back("Bulk job completes server-side first; result pages stream lazily; "
                        "LIMIT is not server-side");
    }
    if (c.count_pushdown) {
        hints.push_back(StringUtil::Format(
            "count pushdown: emitted %lld rows from SELECT COUNT(), no records fetched",
            (long long)c.rows_emitted));
    }
    if (hints.empty()) {
        hints.push_back("ok: predicate + projection pushed down");
    }
    return StringUtil::Join(hints, "; ");
}

struct QueryCostGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> QueryCostBind(ClientContext &, TableFunctionBindInput &,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    names = {"object",          "soql",
             "transport",       "est_rows",
             "transport_reason", "projected_fields",
             "total_fields",    "pushed_filters",
             "residual_filters", "where_pushed",
             "pages_fetched",   "rows_emitted",
             "bulk",            "count_pushdown",
             "bulk_chunks",     "query_mode",
             "quota_remaining", "quota_allowed",
             "guidance",        "bulk_polls"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::BIGINT,
                    LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT,
                    LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::BIGINT,
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BIGINT,
                    LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::BOOLEAN,
                    LogicalType::VARCHAR, LogicalType::BIGINT};
    return nullptr;
}

unique_ptr<GlobalTableFunctionState> QueryCostInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<QueryCostGlobalState>();
}

void Str(DataChunk &out, idx_t col, const string &v) {
    FlatVector::GetData<string_t>(out.data[col])[0] = StringVector::AddString(out.data[col], v);
}
void IntOrNull(DataChunk &out, idx_t col, int64_t v) {
    if (v < 0) {
        FlatVector::SetNull(out.data[col], 0, true);
    } else {
        FlatVector::GetData<int64_t>(out.data[col])[0] = v;
    }
}

void QueryCostFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<QueryCostGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    ScanCost c;
    {
        std::lock_guard<std::mutex> g(g_lock);
        c = g_cost;
    }
    Str(output, 0, c.object);
    Str(output, 1, c.soql);
    Str(output, 2, c.transport);
    IntOrNull(output, 3, c.est_rows);
    Str(output, 4, c.transport_reason);
    FlatVector::GetData<int64_t>(output.data[5])[0] = c.projected_fields;
    FlatVector::GetData<int64_t>(output.data[6])[0] = c.total_fields;
    FlatVector::GetData<int64_t>(output.data[7])[0] = c.pushed_filters;
    FlatVector::GetData<int64_t>(output.data[8])[0] = c.residual_filters;
    Str(output, 9, c.where_pushed);
    IntOrNull(output, 10, c.pages_fetched);
    FlatVector::GetData<int64_t>(output.data[11])[0] = c.rows_emitted;
    FlatVector::GetData<bool>(output.data[12])[0] = c.bulk;
    FlatVector::GetData<bool>(output.data[13])[0] = c.count_pushdown;
    FlatVector::GetData<int64_t>(output.data[14])[0] = c.bulk_chunks;
    Str(output, 15, c.query_mode);
    if (c.quota_consulted) {
        IntOrNull(output, 16, c.quota_remaining);
        FlatVector::GetData<bool>(output.data[17])[0] = c.quota_allowed;
    } else {
        FlatVector::SetNull(output.data[16], 0, true);
        FlatVector::SetNull(output.data[17], 0, true);
    }
    Str(output, 18, BuildGuidance(c));
    IntOrNull(output, 19, c.bulk_polls);
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

void DiagRecordScan(const string &object, const string &soql, const string &transport,
                    int64_t est_rows, const string &transport_reason, int64_t projected_fields,
                    int64_t total_fields, int64_t pushed_filters, int64_t residual_filters,
                    const string &where_pushed, bool bulk, int64_t pages_init,
                    bool count_pushdown, const string &query_mode) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost = ScanCost{};
    g_cost.object = object;
    g_cost.soql = soql;
    g_cost.transport = transport;
    g_cost.est_rows = est_rows;
    g_cost.transport_reason = transport_reason;
    g_cost.projected_fields = projected_fields;
    g_cost.total_fields = total_fields;
    g_cost.pushed_filters = pushed_filters;
    g_cost.residual_filters = residual_filters;
    g_cost.where_pushed = where_pushed;
    g_cost.bulk = bulk;
    g_cost.pages_fetched = pages_init;
    g_cost.count_pushdown = count_pushdown;
    g_cost.query_mode = query_mode;
}

void DiagSetExplain(const string &catalog_alias, vector<DiagExplainItem> items) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.catalog_alias = catalog_alias;
    g_cost.explain_items = std::move(items);
}

DiagExplainSnapshot DiagGetExplain() {
    std::lock_guard<std::mutex> g(g_lock);
    DiagExplainSnapshot snap;
    snap.object = g_cost.object;
    snap.catalog_alias = g_cost.catalog_alias;
    snap.items = g_cost.explain_items;
    snap.transport = g_cost.transport;
    snap.transport_reason = g_cost.transport_reason;
    snap.count_pushdown = g_cost.count_pushdown;
    snap.query_mode = g_cost.query_mode;
    snap.est_rows = g_cost.est_rows;
    return snap;
}

void DiagSetPages(int64_t pages) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.pages_fetched = pages;
}

void DiagSetBulkChunks(int64_t chunks) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.bulk_chunks = chunks;
}

void DiagSetBulkPolls(int64_t polls) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.bulk_polls = polls;
}

void DiagAddRowsEmitted(int64_t rows) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.rows_emitted += rows;
}

void DiagSetQuota(int64_t remaining, bool allowed) {
    std::lock_guard<std::mutex> g(g_lock);
    g_cost.quota_consulted = true;
    g_cost.quota_remaining = remaining;
    g_cost.quota_allowed = allowed;
}

TableFunction GetSalesforceQueryCostFunction() {
    return TableFunction("salesforce_query_cost", {}, QueryCostFunction, QueryCostBind,
                         QueryCostInit);
}

} // namespace duckdb

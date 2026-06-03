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
#include "salesforce_diag.hpp"
#include "salesforce_http.hpp"
#include "salesforce_quota.hpp"
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
    r->pushed_filter_count = pushed_filter_count;
    r->residual_filter_count = residual_filter_count;
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

    // Bulk path (sf_force_transport='bulk'). Lazy result streaming (#v0.7 §8):
    // job created + polled in InitGlobal; result CSV pages fetched ON DEMAND.
    bool bulk = false;
    string bulk_results_base;        // <job>/results (set after the job completes)
    bool bulk_started = false;       // first page fetched yet?
    bool bulk_done = false;          // no more pages after the current is drained
    string bulk_next_locator;        // Sforce-Locator for the next page
    vector<string> bulk_columns;     // CSV header (from the first page)
    vector<vector<string>> bulk_page; // current page's data rows
    idx_t bulk_cursor = 0;            // index into bulk_page
    idx_t bulk_pages = 0;             // result pages fetched
    std::unordered_set<string> bulk_seen; // locator loop guard
    vector<int64_t> field_to_csv;    // field index -> CSV column index (-1 if absent)

    // COUNT pushdown (#v0.5 §5): an aggregate-only scan (zero real columns, no
    // residual filter) emits `count_total` empty rows from a single SELECT
    // COUNT() instead of paging records. DuckDB's COUNT(*) counts them.
    bool count_only = false;
    int64_t count_total = 0;
    int64_t count_cursor = 0;

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
            const auto &f = bind.fields[col];
            if (f.is_relationship) {
                // Parent STRUCT (#v0.6 §7): emit dotted child fields (over-fetch
                // all parent scalars — struct-subfield projection isn't exposed).
                for (auto &child : f.children) {
                    select_fields.push_back(f.relationship_name + "." + child.name);
                }
            } else {
                select_fields.push_back(f.name);
            }
        }
    }
    if (select_fields.empty() && !bind.fields.empty()) {
        select_fields.push_back(bind.fields[0].name);
    }

    // Aggregate-only scan: DuckDB asked for ZERO real columns (COUNT(*),
    // SELECT 1, EXISTS-style). Then the scan's only contract is its row COUNT —
    // used for transport selection (#v0.3 §2) and COUNT pushdown (#v0.5 §5).
    bool aggregate_only = true;
    for (auto col : input.column_ids) {
        if (col < bind.fields.size()) {
            aggregate_only = false;
            break;
        }
    }

    // Predicate pushdown: the WHERE was translated in pushdown_complex_filter
    // (#9); untranslated predicates remain in the plan and DuckDB applies them
    // residually, so results are always correct.
    //
    // LIMIT pushdown is not wired: this DuckDB build does not expose the query
    // LIMIT to a table function, so LIMIT is applied residually by DuckDB.
    string soql = BuildSelectSoql(bind.object, select_fields, bind.pushed_where, optional_idx());
    SetLastSoql(soql);

    // Transport: 'rest' (default, lazy), 'bulk' (Bulk API 2.0), or 'auto' (probe
    // the row count, pick rest/bulk by threshold). Same optimized SOQL either
    // way, so projection/predicate pushdown applies to both.
    string transport = "rest";
    Value tv;
    if (context.TryGetCurrentSetting("sf_force_transport", tv) && !tv.IsNull()) {
        transport = StringUtil::Lower(tv.ToString());
    }
    if (transport != "rest" && transport != "bulk" && transport != "auto") {
        throw BinderException("sf_force_transport must be 'rest', 'bulk' or 'auto' (got '%s').",
                              transport);
    }

    gstate->client = BuildHttpClientForContext(context);
    gstate->session = make_uniq<SalesforceSession>(bind.config, *gstate->client);
    gstate->session->SetToken(bind.token); // reuse ATTACH token (refreshes on 401)
    SetLastScanPages(0);

    // Resolve 'auto' -> 'rest'|'bulk' ONCE here (no mid-stream escalation: that
    // would duplicate already-emitted rows). LIMIT is invisible to a table
    // function, so 'auto' cannot see a small LIMIT — for interactive small-LIMIT
    // reads on a huge object, force sf_force_transport='rest'.
    string effective = transport;
    int64_t est_rows = -1;     // -1 -> no probe ran (NULL in the diagnostic)
    string reason = "forced";  // overwritten on the 'auto' path
    if (transport == "auto") {
        // Aggregate-only scan (COUNT(*) etc.): no real field projected. A Bulk
        // job is pointless here, so stay on REST.
        if (aggregate_only) {
            effective = "rest";
            reason = "auto: aggregate-only -> rest";
        } else {
            bool probe = true;
            Value pv;
            if (context.TryGetCurrentSetting("sf_auto_probe", pv) && !pv.IsNull()) {
                probe = pv.GetValue<bool>();
            }
            if (!probe) {
                effective = "rest";
                reason = "auto: probe disabled -> rest";
            } else {
                int64_t threshold = 50000;
                Value thv;
                if (context.TryGetCurrentSetting("sf_auto_bulk_threshold", thv) && !thv.IsNull()) {
                    threshold = thv.GetValue<int64_t>();
                }
                // COUNT() with the SAME pushed WHERE, so the estimate matches
                // what the scan will read. One REST call, zero row egress.
                string count_soql = "SELECT COUNT() FROM " + bind.object;
                if (!bind.pushed_where.empty()) {
                    count_soql += " WHERE " + bind.pushed_where;
                }
                int64_t n = 0;
                if (gstate->session->TryEstimateCount(count_soql, n)) {
                    est_rows = n;
                    effective = (n > threshold) ? "bulk" : "rest";
                    reason = StringUtil::Format("auto: est %lld rows %s threshold %lld -> %s",
                                                (long long)n, (n > threshold) ? ">" : "<=",
                                                (long long)threshold, effective.c_str());
                } else {
                    effective = "rest"; // probe failed -> safe default, never block
                    reason = "auto: probe failed -> rest";
                }
            }
        }
    }
    SetLastTransport(effective, est_rows, reason);

    // COUNT pushdown (#v0.5 §5): a zero-real-column scan with NO residual filter
    // needs only the row count. Run a single SELECT COUNT() and emit that many
    // empty rows (see ScanFunction) instead of paging records. Forced 'bulk'
    // honours the force and is NOT overridden. Any uncertainty (probe failure)
    // falls back to the normal scan, which is always correct.
    string reported_soql = soql;
    // Bulk now streams pages lazily (#8), so it reports a real page count too.
    int64_t reported_pages = 0;
    if (aggregate_only && bind.residual_filter_count == 0 && effective != "bulk") {
        string count_soql = "SELECT COUNT() FROM " + bind.object;
        if (!bind.pushed_where.empty()) {
            count_soql += " WHERE " + bind.pushed_where;
        }
        int64_t n = 0;
        if (gstate->session->TryEstimateCount(count_soql, n)) {
            gstate->count_only = true;
            gstate->count_total = n;
            reported_soql = count_soql;
            reported_pages = 0;
            SetLastSoql(count_soql);    // reflect the COUNT() SOQL actually sent
            SetLastScanPages(0);        // no data pages fetched
        }
        // probe failed -> leave count_only=false; fall through to the normal scan
    }

    // Query-cost diagnostics (#v0.4 §4 / §5): record the per-scan facts now.
    // pages = 0 for REST/COUNT, NULL (-1) for Bulk (Bulk paging is internal).
    DiagRecordScan(bind.object, reported_soql, effective, est_rows, reason,
                   static_cast<int64_t>(select_fields.size()),
                   static_cast<int64_t>(bind.fields.size()), bind.pushed_filter_count,
                   bind.residual_filter_count, bind.pushed_where, effective == "bulk",
                   reported_pages, gstate->count_only);

    if (gstate->count_only) {
        return std::move(gstate); // no records to fetch; ScanFunction emits the count
    }

    if (effective == "bulk") {
        gstate->bulk = true;
        // Quota governor (#v0.4): gate the Bulk job START on the org's API
        // allocation. REST scans are intentionally not gated.
        QuotaGuardBulkStart(context, *gstate->session,
                            gstate->session->Token().instance_url);
        // Create + poll the job to JobComplete; fetch NO result pages yet (#8).
        // The header / field_to_csv mapping is built lazily from the first page.
        gstate->bulk_results_base = gstate->session->BulkStartJob(soql);
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
        DiagSetPages(static_cast<int64_t>(g.pages_fetched));
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

// Fetch the next Bulk result page lazily (#v0.7 §8). Mirrors ScanAdvancePage:
// fetches one /results page on demand, following the Sforce-Locator, with the
// same loop + max-page guards. Returns false when there are no more rows.
static constexpr idx_t kBulkMaxPages = 1000000;
static bool BulkAdvancePage(ScanGlobalState &g) {
    while (true) {
        if (g.bulk_done) {
            return false;
        }
        string path = g.bulk_started ? (g.bulk_results_base + "?locator=" + g.bulk_next_locator)
                                      : g.bulk_results_base;
        SalesforceBulkPage pg = g.session->BulkFetchResultPage(path);
        g.bulk_started = true;
        g.bulk_pages++;
        SetLastScanPages(g.bulk_pages);
        DiagSetPages(static_cast<int64_t>(g.bulk_pages));
        if (g.bulk_columns.empty() && !pg.columns.empty()) {
            g.bulk_columns = std::move(pg.columns); // header from the first page
        }
        g.bulk_page = std::move(pg.rows);
        g.bulk_cursor = 0;

        if (pg.next_locator.empty()) {
            g.bulk_done = true;
        } else {
            if (g.bulk_pages >= kBulkMaxPages) {
                throw IOException("salesforce bulk: exceeded the maximum result page count.");
            }
            if (!g.bulk_seen.insert(pg.next_locator).second) {
                throw IOException("salesforce bulk: result pagination loop (locator repeated).");
            }
            g.bulk_next_locator = pg.next_locator;
        }

        if (!g.bulk_page.empty()) {
            return true;
        }
        if (g.bulk_done) {
            return false; // empty final page
        }
        // empty non-final page -> loop to fetch the next one
    }
}

// Decode a parent-relationship STRUCT (#v0.6 §7) from Bulk CSV columns named
// "<relationship>.<child>". A row with every child cell missing/empty -> null
// struct; a missing/empty child -> null entry.
static void AppendBulkStruct(Vector &vec, idx_t row, const SalesforceField &field,
                             const vector<string> &cells, const vector<string> &columns) {
    auto &entries = StructVector::GetEntries(vec);
    bool any = false;
    for (idx_t c = 0; c < field.children.size() && c < entries.size(); c++) {
        string header = field.relationship_name + "." + field.children[c].name;
        int64_t ci = -1;
        for (idx_t k = 0; k < columns.size(); k++) {
            if (StringUtil::CIEquals(columns[k], header)) {
                ci = static_cast<int64_t>(k);
                break;
            }
        }
        if (ci < 0 || static_cast<idx_t>(ci) >= cells.size() || cells[ci].empty()) {
            FlatVector::SetNull(*entries[c], row, true);
        } else {
            AppendTypedCell(*entries[c], row, field.children[c], cells[ci]);
            any = true;
        }
    }
    if (!any) {
        FlatVector::SetNull(vec, row, true); // whole parent absent -> null struct
    }
}

static void ScanFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<SalesforceScanBindData>();
    auto &gstate = data.global_state->Cast<ScanGlobalState>();

    // COUNT pushdown (#v0.5 §5): emit count_total empty rows (all-NULL virtual
    // columns) so DuckDB's COUNT(*) counts the right number — no records fetched.
    if (gstate.count_only) {
        idx_t row = 0;
        while (row < STANDARD_VECTOR_SIZE && gstate.count_cursor < gstate.count_total) {
            for (idx_t j = 0; j < gstate.column_ids.size(); j++) {
                FlatVector::SetNull(output.data[j], row, true);
            }
            gstate.count_cursor++;
            row++;
        }
        output.SetCardinality(row);
        DiagAddRowsEmitted(static_cast<int64_t>(row));
        return;
    }

    // Bulk path: stream decoded CSV rows, fetching the next /results page only
    // when the current one is drained (#v0.7 §8 — lazy, page granularity so a
    // small LIMIT stops pulling before later pages are downloaded).
    if (gstate.bulk) {
        if (gstate.bulk_cursor >= gstate.bulk_page.size()) {
            if (!BulkAdvancePage(gstate)) {
                output.SetCardinality(0);
                return;
            }
            // Build the field -> CSV column map once the first header is known.
            if (gstate.field_to_csv.empty() && !gstate.bulk_columns.empty()) {
                gstate.field_to_csv.assign(bind.fields.size(), -1);
                for (idx_t f = 0; f < bind.fields.size(); f++) {
                    for (idx_t c = 0; c < gstate.bulk_columns.size(); c++) {
                        if (StringUtil::CIEquals(bind.fields[f].name, gstate.bulk_columns[c])) {
                            gstate.field_to_csv[f] = static_cast<int64_t>(c);
                            break;
                        }
                    }
                }
            }
        }
        idx_t row = 0;
        while (row < STANDARD_VECTOR_SIZE && gstate.bulk_cursor < gstate.bulk_page.size()) {
            const auto &cells = gstate.bulk_page[gstate.bulk_cursor];
            for (idx_t j = 0; j < gstate.column_ids.size(); j++) {
                column_t col = gstate.column_ids[j];
                if (col < bind.fields.size() && bind.fields[col].is_relationship) {
                    // Parent STRUCT from CSV columns "rel.child" (#v0.6 §7).
                    AppendBulkStruct(output.data[j], row, bind.fields[col], cells,
                                     gstate.bulk_columns);
                    continue;
                }
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
        DiagAddRowsEmitted(static_cast<int64_t>(row)); // rows delivered to DuckDB
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
    DiagAddRowsEmitted(static_cast<int64_t>(row)); // rows delivered to DuckDB
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
    // DuckDB may invoke this hook MORE THAN ONCE (e.g. for an aggregate plan it
    // calls again with an empty list after the first call consumed the filter).
    // PushdownToSoql clears its out_where, so a naive call would WIPE the WHERE
    // built by an earlier call. Skip empty calls and ACCUMULATE instead, so the
    // pushed WHERE survives — otherwise COUNT(*) ... WHERE silently over-counts.
    idx_t before = filters.size();
    if (before == 0) {
        return;
    }
    string where_part;
    PushdownToSoql(bind.fields, projection_to_field, where_part, filters);
    if (!where_part.empty()) {
        bind.pushed_where =
            bind.pushed_where.empty() ? where_part : bind.pushed_where + " AND " + where_part;
    }
    // Translated filters were removed; the rest stay residual for DuckDB.
    // Recorded for salesforce_query_cost() (#v0.4 §4).
    bind.pushed_filter_count += static_cast<int64_t>(before - filters.size());
    bind.residual_filter_count = static_cast<int64_t>(filters.size());
}

} // namespace

TableFunction GetSalesforceScanFunction() {
    TableFunction fn("salesforce_scan", {}, ScanFunction, ScanBind, ScanInitGlobal);
    fn.projection_pushdown = true; // DuckDB-level column projection
    fn.pushdown_complex_filter = ScanPushdownComplexFilter; // SOQL WHERE, residual-safe
    return fn;
}

} // namespace duckdb

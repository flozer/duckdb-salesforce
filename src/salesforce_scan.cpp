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

#include <atomic>
#include <limits>
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

    // Bulk path (sf_force_transport='bulk'). Lazy result streaming (#v0.7 §8) +
    // PK chunking (#v0.7 §9). The per-chunk job + result streaming live in the
    // PER-THREAD local state (#v0.7 §9b parallel); the global state only holds
    // the chunk work-list, hands out chunk indices, and aggregates diagnostics.
    bool bulk = false;
    bool query_all = false;                   // #v0.9 §1: queryAll mode (per-thread sessions read this)
    int bulk_poll_budget = 600;               // ROADMAP §15: per-thread Bulk sessions read this
    vector<string> bulk_chunk_soqls;          // full SOQL per chunk (built in InitGlobal)
    std::atomic<idx_t> next_chunk{0};         // dispenser: next chunk index to claim
    std::atomic<idx_t> total_bulk_pages{0};   // aggregate result pages (all threads)

    // COUNT pushdown (#v0.5 §5): an aggregate-only scan (zero real columns, no
    // residual filter) emits `count_total` empty rows from a single SELECT
    // COUNT() instead of paging records. DuckDB's COUNT(*) counts them.
    bool count_only = false;
    int64_t count_total = 0;
    int64_t count_cursor = 0;

    // DuckDB-level projection: which source field each output column maps to.
    vector<column_t> column_ids;
    idx_t max_threads = 1;
    idx_t MaxThreads() const override {
        return max_threads;
    }
};

// Per-thread state (#v0.7 §9b). Each thread claims chunk indices from the
// global dispenser and owns its OWN client/session/job + lazy page streaming,
// so chunks run in parallel with isolated HTTP lifecycle. Used only on the Bulk
// path; REST/COUNT run on the global state (MaxThreads=1).
struct ScanLocalState : public LocalTableFunctionState {
    // client declared before session (reverse-order destruction).
    unique_ptr<SalesforceHttpClient> client;
    unique_ptr<SalesforceSession> session;
    bool has_chunk = false;          // a job is started + streaming for this chunk
    string results_base;             // <job>/results
    bool started = false;            // first page fetched yet (current chunk)?
    bool chunk_done = false;         // current chunk drained
    string next_locator;             // Sforce-Locator for the next page
    vector<string> columns;          // CSV header (from the first page)
    vector<vector<string>> page;     // current page's data rows
    idx_t cursor = 0;                // index into page
    std::unordered_set<string> seen; // locator loop guard (current chunk)
    vector<int64_t> field_to_csv;    // field index -> CSV column index (-1 if absent)
};

// --- PK chunking helpers (#v0.7 §9) -----------------------------------------
// Salesforce Id alphabet in ASCII/lexical order: 0-9 < A-Z < a-z. Mapping digit
// values to this order makes base62 value-order match string lexical order, so
// interpolated boundaries are lexically monotonic.
static int Base62Val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    return 0;
}
static char Base62Char(int v) {
    if (v < 10) return static_cast<char>('0' + v);
    if (v < 36) return static_cast<char>('A' + v - 10);
    return static_cast<char>('a' + v - 36);
}
// We interpolate over the first kIdPrec chars only (62^10 < 2^63 — enough
// resolution for <=8 chunks); the boundary is then padded to the FULL Salesforce
// Id length so the generated `Id < '...'` is a syntactically valid Id (15 or 18
// chars). The shared object key prefix (e.g. 001) is inside these high digits,
// so it is preserved automatically. NEVER emit a short/partial boundary (#27).
static constexpr int kIdPrec = 10;
static uint64_t IdToNum(const string &id) {
    uint64_t v = 0;
    for (int i = 0; i < kIdPrec; i++) {
        v = v * 62 + (static_cast<size_t>(i) < id.size() ? Base62Val(id[i]) : 0);
    }
    return v;
}
// Produce a length-`len` Id boundary: the kIdPrec interpolated chars, right-
// padded with '0' (lowest base62 value, keeps lexical ordering) to `len`.
static string NumToId(uint64_t v, idx_t len) {
    string prefix(kIdPrec, '0');
    for (int i = kIdPrec - 1; i >= 0; i--) {
        prefix[i] = Base62Char(static_cast<int>(v % 62));
        v /= 62;
    }
    if (len <= static_cast<idx_t>(kIdPrec)) {
        return prefix.substr(0, len);
    }
    prefix.append(len - kIdPrec, '0'); // pad suffix to a valid Id length
    return prefix;
}

// Build N disjoint, exhaustive WHERE clauses over [min_id, max_id] by uniform
// lexical interpolation (chunks may be uneven/empty — acceptable, cut 1). Each
// is combined (AND) with the pushed WHERE. Coverage: chunk 0 starts at min_id,
// the last ends inclusive at max_id, interior boundaries strictly increasing.
static vector<string> BuildIdRangeWheres(const string &pushed, const string &min_id,
                                         const string &max_id, int64_t n) {
    auto combine = [&](const string &range) {
        return pushed.empty() ? range : ("(" + pushed + ") AND (" + range + ")");
    };
    uint64_t lo_num = IdToNum(min_id), hi_num = IdToNum(max_id);
    // Interior boundaries are padded to the real Id length (15/18) so they are
    // valid Salesforce Ids; the outer bounds are the actual MIN/MAX(Id) (#27).
    idx_t id_len = min_id.size() >= max_id.size() ? min_id.size() : max_id.size();
    vector<string> b;
    b.push_back(min_id);
    for (int64_t i = 1; i < n; i++) {
        uint64_t v = lo_num + (hi_num - lo_num) * static_cast<uint64_t>(i) / static_cast<uint64_t>(n);
        b.push_back(NumToId(v, id_len));
    }
    b.push_back(max_id);
    vector<string> wheres;
    for (int64_t i = 0; i < n; i++) {
        const string &lo = b[i];
        const string &hi = b[i + 1];
        if (i > 0 && hi <= lo) {
            continue; // collapsed boundary -> skip (fewer effective chunks)
        }
        string range;
        if (i == n - 1) {
            range = "Id >= '" + lo + "' AND Id <= '" + hi + "'"; // last: inclusive max
        } else {
            range = "Id >= '" + lo + "' AND Id < '" + hi + "'";
        }
        wheres.push_back(combine(range));
    }
    return wheres;
}

// Emit SOQL dotted paths for a relationship STRUCT field, recursing into nested
// (grandparent) relationship children (#v1.0). `path` is the dotted prefix so
// far (e.g. "Account", then "Account.Owner").
static void EmitRelationshipSoql(const SalesforceField &rel, const string &path,
                                 vector<string> &out) {
    for (auto &child : rel.children) {
        if (child.is_relationship) {
            EmitRelationshipSoql(child, path + "." + child.name, out);
        } else {
            out.push_back(path + "." + child.name);
        }
    }
}

static unique_ptr<FunctionData> ScanBind(ClientContext &, TableFunctionBindInput &,
                                         vector<LogicalType> &, vector<string> &) {
    throw InternalException(
        "salesforce_scan is catalog-internal and cannot be called directly");
}

// First projected base64/blob field (dotted path for a nested parent field),
// or "" if none. Live-confirmed: Bulk API 2.0 query CSV rejects blob fields
// ("Blob field not supported in Bulk V2 Query with CSV content type"), so a
// projected base64 field makes the Bulk transport incompatible (#v1.3 §11).
static string FindProjectedBase64(const SalesforceField &f, const string &prefix) {
    if (f.is_relationship) {
        for (auto &c : f.children) {
            string r = FindProjectedBase64(c, prefix + f.relationship_name + ".");
            if (!r.empty()) {
                return r;
            }
        }
        return "";
    }
    if (StringUtil::Lower(f.sf_type) == "base64") {
        return prefix + f.name;
    }
    return "";
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
                // Parent STRUCT (#v0.6 §7 / depth-2 #v1.0): emit dotted child
                // fields recursively (Account.Name, Account.Owner.Name) —
                // over-fetches all scalars at every level.
                EmitRelationshipSoql(f, f.relationship_name, select_fields);
            } else {
                select_fields.push_back(f.name);
            }
        }
    }
    if (select_fields.empty() && !bind.fields.empty()) {
        select_fields.push_back(bind.fields[0].name);
    }

    // Bulk compatibility guard (#v1.3 §11): a projected base64/blob field is not
    // returned by Bulk API 2.0 query CSV (live-confirmed). Detected from
    // metadata; used below to keep 'auto' off Bulk and to reject a forced
    // 'bulk' with a clear error before any job is created.
    string base64_field;
    for (auto col : input.column_ids) {
        if (col >= bind.fields.size()) {
            continue;
        }
        base64_field = FindProjectedBase64(bind.fields[col], "");
        if (!base64_field.empty()) {
            break;
        }
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

    // Read mode (#v0.9 §1): 'query' (default) or 'queryAll' (incl. archived +
    // soft-deleted). Applied to REST, Bulk, and the COUNT()/MIN-MAX probes.
    string query_mode = "query";
    Value qmv;
    if (context.TryGetCurrentSetting("sf_query_mode", qmv) && !qmv.IsNull()) {
        query_mode = StringUtil::Lower(qmv.ToString());
    }
    if (query_mode != "query" && query_mode != "queryall") {
        throw BinderException("sf_query_mode must be 'query' or 'queryAll' (got '%s').",
                              query_mode);
    }
    gstate->query_all = (query_mode == "queryall");

    gstate->client = BuildHttpClientForContext(context);
    gstate->session = make_uniq<SalesforceSession>(bind.config, *gstate->client);
    gstate->session->SetToken(bind.token); // reuse ATTACH token (refreshes on 401)
    gstate->session->SetQueryAll(gstate->query_all); // probes honour the mode too
    // Bulk poll budget (ROADMAP §15): how many job-status polls BulkStartJob may
    // run before failing fast. Default 600 keeps prior behaviour.
    Value pbv;
    if (context.TryGetCurrentSetting("sf_bulk_poll_budget", pbv) && !pbv.IsNull()) {
        // Clamp into int range BEFORE the cast: a BIGINT setting can exceed
        // INT_MAX, and a raw static_cast would wrap (possibly negative).
        int64_t b = pbv.GetValue<int64_t>();
        if (b < 1) {
            b = 1;
        } else if (b > std::numeric_limits<int>::max()) {
            b = std::numeric_limits<int>::max();
        }
        gstate->bulk_poll_budget = static_cast<int>(b);
    }
    gstate->session->SetBulkPollBudget(gstate->bulk_poll_budget);
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

    // Apply the Bulk compatibility guard (#v1.3 §11). A forced 'bulk' on a
    // projected blob field is a hard error (no silent fallback, no job created);
    // 'auto' that landed on Bulk falls back to REST with a recorded reason.
    if (!base64_field.empty()) {
        if (transport == "bulk") {
            throw BinderException(
                "projected base64 field '%s' is not supported by Bulk API 2.0 CSV; "
                "use 'rest' or 'auto'.",
                base64_field);
        }
        if (effective == "bulk") {
            effective = "rest";
            reason = StringUtil::Format(
                "auto: bulk-incompatible (projected base64 field '%s') -> rest",
                base64_field);
        }
    }
    SetLastTransport(effective, est_rows, reason);

    // Bulk backfill guardrail (ROADMAP §15): a Bulk read with NO predicate pushed
    // to SOQL is a full-object extraction — the exact shape that exhausts the
    // poll budget on large objects. Off by default (guidance only); when
    // sf_bulk_require_predicate is enabled, fail fast before creating the job so
    // a planned large backfill must prove a pushed CreatedDate/SystemModstamp
    // window first.
    if (effective == "bulk" && bind.pushed_where.empty()) {
        Value rpv;
        if (context.TryGetCurrentSetting("sf_bulk_require_predicate", rpv) && !rpv.IsNull() &&
            rpv.GetValue<bool>()) {
            throw BinderException(
                "Bulk read on '%s' has no pushed predicate (full-object extraction). "
                "sf_bulk_require_predicate is on: add a server-filterable WHERE — e.g. a "
                "CreatedDate/SystemModstamp range — or disable the guard to proceed.",
                bind.object);
        }
    }

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
                   reported_pages, gstate->count_only,
                   gstate->query_all ? "queryAll" : "query");

    if (gstate->count_only) {
        return std::move(gstate); // no records to fetch; ScanFunction emits the count
    }

    if (effective == "bulk") {
        gstate->bulk = true;
        ResetBulkCreateBodies(); // accumulate one create-body per chunk job (#9)
        // PK chunking (#v0.7 §9): split into N Id-range chunks when asked + the
        // MIN/MAX probe succeeds; else a single chunk = the original SOQL. Jobs
        // are NOT started here — each chunk's job starts lazily in ScanFunction
        // (quota-gated per job), streamed via §8.
        int64_t chunks = 1;
        Value cv;
        if (context.TryGetCurrentSetting("sf_bulk_chunks", cv) && !cv.IsNull()) {
            chunks = cv.GetValue<int64_t>();
        }
        if (chunks < 1) {
            chunks = 1;
        }
        if (chunks > 8) {
            chunks = 8; // cap (cut 1)
        }
        vector<string> wheres;
        if (chunks > 1) {
            string mn, mx;
            if (gstate->session->TryMinMaxId(bind.object, bind.pushed_where, mn, mx)) {
                wheres = BuildIdRangeWheres(bind.pushed_where, mn, mx, chunks);
            }
        }
        if (wheres.empty()) {
            wheres.push_back(bind.pushed_where); // single chunk (no/failed chunking)
        }
        for (auto &w : wheres) {
            gstate->bulk_chunk_soqls.push_back(
                BuildSelectSoql(bind.object, select_fields, w, optional_idx()));
        }
        DiagSetBulkChunks(static_cast<int64_t>(gstate->bulk_chunk_soqls.size()));
        // Parallelise across chunks (#v0.7 §9b): up to one thread per chunk.
        gstate->max_threads = gstate->bulk_chunk_soqls.size();
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

static constexpr idx_t kBulkMaxPages = 1000000;
// Fetch the next page of the LOCAL state's current chunk; aggregate the page
// count into the GLOBAL atomic (#v0.7 §9b). Returns false when the current chunk
// has no more rows (caller then claims the next chunk).
static bool BulkAdvancePage(ScanGlobalState &g, ScanLocalState &l) {
    while (true) {
        if (l.chunk_done) {
            return false;
        }
        string path =
            l.started ? (l.results_base + "?locator=" + l.next_locator) : l.results_base;
        SalesforceBulkPage pg = l.session->BulkFetchResultPage(path);
        l.started = true;
        idx_t total = g.total_bulk_pages.fetch_add(1) + 1;
        SetLastScanPages(static_cast<int64_t>(total));
        DiagSetPages(static_cast<int64_t>(total));
        if (l.columns.empty() && !pg.columns.empty()) {
            l.columns = std::move(pg.columns); // header from the first page
        }
        l.page = std::move(pg.rows);
        l.cursor = 0;

        if (pg.next_locator.empty()) {
            l.chunk_done = true;
        } else {
            if (total >= kBulkMaxPages) {
                throw IOException("salesforce bulk: exceeded the maximum result page count.");
            }
            if (!l.seen.insert(pg.next_locator).second) {
                throw IOException("salesforce bulk: result pagination loop (locator repeated).");
            }
            l.next_locator = pg.next_locator;
        }

        if (!l.page.empty()) {
            return true;
        }
        if (l.chunk_done) {
            return false; // empty final page of this chunk
        }
        // empty non-final page -> loop to fetch the next one
    }
}

// Decode a parent-relationship STRUCT (#v0.6 §7, depth-2 #v1.0) from Bulk CSV
// columns named "<path>.<child>" (e.g. "Account.Name", "Account.Owner.Name").
// `path` is the dotted prefix. Nested relationship children recurse. A struct
// with no populated descendant cell -> null struct; missing/empty cell -> null.
static void AppendBulkStruct(Vector &vec, idx_t row, const SalesforceField &field,
                             const string &path, const vector<string> &cells,
                             const vector<string> &columns) {
    auto &entries = StructVector::GetEntries(vec);
    bool any = false;
    for (idx_t c = 0; c < field.children.size() && c < entries.size(); c++) {
        const auto &child = field.children[c];
        if (child.is_relationship) {
            AppendBulkStruct(*entries[c], row, child, path + "." + child.name, cells, columns);
            if (!FlatVector::IsNull(*entries[c], row)) {
                any = true;
            }
            continue;
        }
        string header = path + "." + child.name;
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
            AppendTypedCell(*entries[c], row, child, cells[ci]);
            any = true;
        }
    }
    if (!any) {
        FlatVector::SetNull(vec, row, true); // whole parent absent -> null struct
    }
}

static void ScanFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
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

    // Bulk path (#v0.7 §9b parallel): this thread's LOCAL state claims chunk
    // indices from the global dispenser and streams each chunk's pages lazily
    // (§8) over its OWN client/session/job. Chunks run in parallel.
    if (gstate.bulk) {
        auto &lstate = data.local_state->Cast<ScanLocalState>();
        // Lazily build this thread's client + session (Bulk-only, per thread).
        if (!lstate.session) {
            lstate.client = BuildHttpClientForContext(context);
            lstate.session = make_uniq<SalesforceSession>(bind.config, *lstate.client);
            lstate.session->SetToken(bind.token);
            lstate.session->SetQueryAll(gstate.query_all); // #v0.9 §1
            lstate.session->SetBulkPollBudget(gstate.bulk_poll_budget); // ROADMAP §15
        }
        if (lstate.cursor >= lstate.page.size()) {
            // Need a fresh page: advance the current chunk, or claim the next.
            bool got = false;
            while (!got) {
                if (!lstate.has_chunk) {
                    idx_t idx = gstate.next_chunk.fetch_add(1);
                    if (idx >= gstate.bulk_chunk_soqls.size()) {
                        output.SetCardinality(0);
                        return; // no more chunks for this thread
                    }
                    // Start this chunk's Bulk job — quota-gated PER job (#9).
                    QuotaGuardBulkStart(context, *lstate.session,
                                        lstate.session->Token().instance_url);
                    lstate.results_base = lstate.session->BulkStartJob(gstate.bulk_chunk_soqls[idx]);
                    lstate.started = false;
                    lstate.chunk_done = false;
                    lstate.next_locator.clear();
                    lstate.seen.clear();
                    lstate.has_chunk = true;
                }
                if (BulkAdvancePage(gstate, lstate)) {
                    got = true;
                } else {
                    lstate.has_chunk = false; // chunk exhausted -> claim the next
                }
            }
            // Build the field -> CSV column map once the first header is known.
            if (lstate.field_to_csv.empty() && !lstate.columns.empty()) {
                lstate.field_to_csv.assign(bind.fields.size(), -1);
                for (idx_t f = 0; f < bind.fields.size(); f++) {
                    for (idx_t c = 0; c < lstate.columns.size(); c++) {
                        if (StringUtil::CIEquals(bind.fields[f].name, lstate.columns[c])) {
                            lstate.field_to_csv[f] = static_cast<int64_t>(c);
                            break;
                        }
                    }
                }
            }
        }
        idx_t row = 0;
        while (row < STANDARD_VECTOR_SIZE && lstate.cursor < lstate.page.size()) {
            const auto &cells = lstate.page[lstate.cursor];
            for (idx_t j = 0; j < gstate.column_ids.size(); j++) {
                column_t col = gstate.column_ids[j];
                if (col < bind.fields.size() && bind.fields[col].is_relationship) {
                    // Parent STRUCT from CSV columns "rel.child" (#v0.6 §7).
                    AppendBulkStruct(output.data[j], row, bind.fields[col],
                                     bind.fields[col].relationship_name, cells, lstate.columns);
                    continue;
                }
                int64_t ci = (col < bind.fields.size()) ? lstate.field_to_csv[col] : -1;
                if (ci < 0 || static_cast<idx_t>(ci) >= cells.size() || cells[ci].empty()) {
                    FlatVector::SetNull(output.data[j], row, true); // missing/virtual/empty -> NULL
                } else {
                    AppendTypedCell(output.data[j], row, bind.fields[col], cells[ci]);
                }
            }
            lstate.cursor++;
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

// Per-thread local state (#v0.7 §9b). Empty until first use; the Bulk path
// lazily builds its client/session. REST/COUNT ignore it (run on the global).
static unique_ptr<LocalTableFunctionState>
ScanInitLocal(ExecutionContext &, TableFunctionInitInput &, GlobalTableFunctionState *) {
    return make_uniq<ScanLocalState>();
}

} // namespace

TableFunction GetSalesforceScanFunction() {
    TableFunction fn("salesforce_scan", {}, ScanFunction, ScanBind, ScanInitGlobal, ScanInitLocal);
    fn.projection_pushdown = true; // DuckDB-level column projection
    fn.pushdown_complex_filter = ScanPushdownComplexFilter; // SOQL WHERE, residual-safe
    return fn;
}

} // namespace duckdb

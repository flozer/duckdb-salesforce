// Relationship diagnostics (#v1.0). One consolidated LAST-RESOLUTION snapshot
// of parent-relationship expansion + the salesforce_relationships() table
// function. Pure in-memory diagnostic; never holds a secret (only object /
// field / relationship names, which are the user's own schema).

#include "salesforce_reldiag.hpp"

#include "duckdb/common/string_util.hpp"

#include <mutex>

namespace duckdb {

namespace {

struct RelDecision {
    string relationship_name;
    string parent_object; // empty -> NULL (polymorphic)
    int64_t depth_level = 1;
    string status; // expanded | skipped
    string reason; // empty -> NULL (expanded)
    int64_t field_count = -1; // -1 -> NULL (skipped)
};

struct RelSnapshot {
    string object;
    string mode = "off";
    int64_t depth = 1;
    vector<RelDecision> decisions;
};

std::mutex g_lock;
RelSnapshot g_snap;

// Over-fetch caveat attached to every expanded relationship row.
const char *kOverfetchNote =
    "parent fetched as a full STRUCT (all queryable parent fields); nested "
    "projection is not pushed into SOQL — selecting one subfield still fetches "
    "the whole parent";

string BuildConfigNote(const RelSnapshot &s, int64_t expanded, int64_t skipped) {
    if (s.mode != "parent") {
        return "relationship expansion disabled (SET sf_relationships='parent' to enable)";
    }
    if (expanded == 0 && skipped == 0) {
        return "relationship expansion on, but this object has no usable parent references";
    }
    return StringUtil::Format(
        "expanded %lld, skipped %lld parent relationship(s) up to depth %lld",
        (long long)expanded, (long long)skipped, (long long)s.depth);
}

struct RelationshipsGlobalState : public GlobalTableFunctionState {
    RelSnapshot snap;
    int64_t expanded = 0;
    int64_t skipped = 0;
    idx_t row_idx = 0; // 0 = config row; 1.. = decisions[row_idx-1]
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> RelationshipsBind(ClientContext &, TableFunctionBindInput &,
                                           vector<LogicalType> &return_types,
                                           vector<string> &names) {
    names = {"row_type",         "object",
             "relationships_mode", "relationship_depth",
             "relationship_name", "parent_object",
             "depth_level",      "status",
             "reason",           "field_count",
             "expanded_count",   "skipped_count",
             "note"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT,
                    LogicalType::VARCHAR};
    return nullptr;
}

unique_ptr<GlobalTableFunctionState> RelationshipsInit(ClientContext &, TableFunctionInitInput &) {
    auto gstate = make_uniq<RelationshipsGlobalState>();
    {
        std::lock_guard<std::mutex> g(g_lock);
        gstate->snap = g_snap;
    }
    for (auto &d : gstate->snap.decisions) {
        if (d.status == "expanded") {
            gstate->expanded++;
        } else {
            gstate->skipped++;
        }
    }
    return std::move(gstate);
}

void SetStr(DataChunk &out, idx_t col, idx_t row, const string &v) {
    FlatVector::GetData<string_t>(out.data[col])[row] = StringVector::AddString(out.data[col], v);
}
void SetStrOrNull(DataChunk &out, idx_t col, idx_t row, const string &v) {
    if (v.empty()) {
        FlatVector::SetNull(out.data[col], row, true);
    } else {
        SetStr(out, col, row, v);
    }
}
void SetInt(DataChunk &out, idx_t col, idx_t row, int64_t v) {
    FlatVector::GetData<int64_t>(out.data[col])[row] = v;
}
void SetIntOrNull(DataChunk &out, idx_t col, idx_t row, int64_t v) {
    if (v < 0) {
        FlatVector::SetNull(out.data[col], row, true);
    } else {
        SetInt(out, col, row, v);
    }
}
void SetNull(DataChunk &out, idx_t col, idx_t row) {
    FlatVector::SetNull(out.data[col], row, true);
}

void RelationshipsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<RelationshipsGlobalState>();
    const auto &snap = gstate.snap;
    const idx_t total = 1 + snap.decisions.size(); // config row + one per decision

    idx_t produced = 0;
    while (gstate.row_idx < total && produced < STANDARD_VECTOR_SIZE) {
        idx_t r = produced;
        if (gstate.row_idx == 0) {
            // Config / summary row. Relationship-specific columns NULL.
            SetStr(output, 0, r, "config");
            SetStr(output, 1, r, snap.object);
            SetStr(output, 2, r, snap.mode);
            SetInt(output, 3, r, snap.depth);
            SetNull(output, 4, r); // relationship_name
            SetNull(output, 5, r); // parent_object
            SetNull(output, 6, r); // depth_level
            SetNull(output, 7, r); // status
            SetNull(output, 8, r); // reason
            SetNull(output, 9, r); // field_count
            SetInt(output, 10, r, gstate.expanded);
            SetInt(output, 11, r, gstate.skipped);
            SetStr(output, 12, r, BuildConfigNote(snap, gstate.expanded, gstate.skipped));
        } else {
            // Relationship decision row. Config-only columns NULL.
            const auto &d = snap.decisions[gstate.row_idx - 1];
            SetStr(output, 0, r, "relationship");
            SetStr(output, 1, r, snap.object);
            SetNull(output, 2, r); // relationships_mode
            SetNull(output, 3, r); // relationship_depth
            SetStr(output, 4, r, d.relationship_name);
            SetStrOrNull(output, 5, r, d.parent_object);
            SetInt(output, 6, r, d.depth_level);
            SetStr(output, 7, r, d.status);
            SetStrOrNull(output, 8, r, d.reason);
            SetIntOrNull(output, 9, r, d.field_count);
            SetNull(output, 10, r); // expanded_count
            SetNull(output, 11, r); // skipped_count
            if (d.status == "expanded") {
                SetStr(output, 12, r, kOverfetchNote);
            } else {
                SetNull(output, 12, r);
            }
        }
        gstate.row_idx++;
        produced++;
    }
    output.SetCardinality(produced);
}

} // namespace

void RelDiagBegin(const string &object, const string &mode, int64_t depth) {
    std::lock_guard<std::mutex> g(g_lock);
    g_snap = RelSnapshot{};
    g_snap.object = object;
    g_snap.mode = mode;
    g_snap.depth = depth;
}

void RelDiagRecord(const string &relationship_name, const string &parent_object,
                   int64_t depth_level, const string &status, const string &reason,
                   int64_t field_count) {
    std::lock_guard<std::mutex> g(g_lock);
    RelDecision d;
    d.relationship_name = relationship_name;
    d.parent_object = parent_object;
    d.depth_level = depth_level;
    d.status = status;
    d.reason = reason;
    d.field_count = field_count;
    g_snap.decisions.push_back(std::move(d));
}

TableFunction GetSalesforceRelationshipsFunction() {
    return TableFunction("salesforce_relationships", {}, RelationshipsFunction, RelationshipsBind,
                         RelationshipsInit);
}

} // namespace duckdb

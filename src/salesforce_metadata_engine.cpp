// Metadata Engine v2 (ROADMAP v1.6 §17) — shared per-catalog read-only metadata
// cache. See salesforce_metadata_engine.hpp. Phase A: data accessors + cache +
// resolution helpers. The token/relationship resolution mirrors the Report
// Bridge rules; Report Bridge migrates onto this engine in Phase B (the
// duplicated helpers below are removed then).

#include "salesforce_metadata_engine.hpp"
#include "salesforce_diag.hpp" // DiagGetExplain (query_explain)
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp" // DEBUG/TEST describe-call counters

#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

// --- token -> field resolution helpers (mirror salesforce_report.cpp) --------

string BuiltinReportToken(const string &token) {
    static const std::pair<const char *, const char *> kMap[] = {
        {"ID", "Id"},          {"NAME", "Name"},          {"FIRST_NAME", "FirstName"},
        {"LAST_NAME", "LastName"}, {"EMAIL", "Email"},     {"PHONE", "Phone"},
        {"CREATED_DATE", "CreatedDate"},
    };
    for (auto &m : kMap) {
        if (StringUtil::CIEquals(token, m.first)) {
            return m.second;
        }
    }
    return "";
}

string NormalizeSnakeToken(const string &token) {
    string out;
    bool start = true;
    for (char c : token) {
        if (c == '_') {
            start = true;
            continue;
        }
        char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        char lo = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        out.push_back(start ? up : lo);
        start = false;
    }
    return out;
}

} // namespace

SalesforceMetadataEngine::SalesforceMetadataEngine(SalesforceConfig config,
                                                   SalesforceTokenSet token)
    : config_(std::move(config)), token_(std::move(token)) {}

SalesforceMetadataEngine::~SalesforceMetadataEngine() = default;

SalesforceSession &SalesforceMetadataEngine::Session(ClientContext &context) {
    if (!session_) {
        client_ = BuildHttpClientForContext(context);
        session_ = make_uniq<SalesforceSession>(config_, *client_);
        session_->SetToken(token_);
    }
    return *session_;
}

void SalesforceMetadataEngine::EnsureGlobalLoaded(ClientContext &context) {
    if (global_loaded_) {
        return;
    }
    IncGlobalDescribeCalls(); // DEBUG/TEST: proves Describe-Global-once
    global_infos_ = Session(context).GlobalDescribeInfos();
    // Derive the queryable-only name list — bit-identical to the legacy
    // GlobalDescribe() filter, so IsQueryable / report_soql / scan are unchanged.
    global_objects_.clear();
    for (auto &info : global_infos_) {
        if (info.queryable) {
            global_objects_.push_back(info.name);
        }
    }
    global_loaded_ = true;
}

const vector<string> &SalesforceMetadataEngine::GetGlobalObjects(ClientContext &context) {
    EnsureGlobalLoaded(context);
    return global_objects_;
}

const vector<SalesforceObjectInfo> &
SalesforceMetadataEngine::GetGlobalObjectInfos(ClientContext &context) {
    EnsureGlobalLoaded(context);
    return global_infos_;
}

const SalesforceDescribe &SalesforceMetadataEngine::GetObjectDescribe(ClientContext &context,
                                                                      const string &object) {
    string key = StringUtil::Lower(object);
    auto it = describe_.find(key);
    if (it != describe_.end()) {
        return it->second;
    }
    IncDescribeCalls(); // DEBUG/TEST: proves describe-once per object
    auto res = describe_.emplace(key, Session(context).Describe(object));
    return res.first->second;
}

bool SalesforceMetadataEngine::IsQueryable(ClientContext &context, const string &object) {
    for (auto &n : GetGlobalObjects(context)) {
        if (StringUtil::CIEquals(n, object)) {
            return true;
        }
    }
    return false;
}

bool SalesforceMetadataEngine::ResolveField(ClientContext &context, const string &object,
                                            const string &token, string &out_realname,
                                            bool &out_filterable) {
    const SalesforceDescribe &desc = GetObjectDescribe(context, object);
    vector<string> cands;
    cands.push_back(token);
    string mapped = BuiltinReportToken(token);
    if (!mapped.empty()) {
        cands.push_back(mapped);
    }
    cands.push_back(NormalizeSnakeToken(token));
    for (auto &c : cands) {
        string lc = StringUtil::Lower(c);
        for (auto &fld : desc.fields) {
            if (StringUtil::Lower(fld.name) == lc) {
                out_realname = fld.name;
                out_filterable = fld.filterable;
                return true;
            }
        }
    }
    return false;
}

bool SalesforceMetadataEngine::ResolveRelationship(ClientContext &context, const string &object,
                                                   const string &relationship_name,
                                                   string &out_target, string &out_real_name) {
    const SalesforceDescribe &desc = GetObjectDescribe(context, object);
    for (auto &fld : desc.fields) {
        if (fld.relationship_name.empty() ||
            !StringUtil::CIEquals(fld.relationship_name, relationship_name)) {
            continue;
        }
        if (fld.reference_to.size() != 1) {
            return false; // polymorphic / none
        }
        const string &target = fld.reference_to[0];
        if (!IsQueryable(context, target)) {
            return false;
        }
        out_target = target;
        out_real_name = fld.relationship_name;
        return true;
    }
    return false;
}

void SalesforceMetadataEngine::Refresh(const string &object) {
    describe_.erase(StringUtil::Lower(object));
    // Drop the cached client/session so the next fetch is rebuilt against the
    // current context (re-reads live/mock settings). Within a single fetch
    // sequence the rebuilt session is reused, so multi-object lookups stay
    // consistent; across refreshes a changed org is seen fresh.
    session_.reset();
    client_.reset();
}

void SalesforceMetadataEngine::RefreshAll() {
    global_loaded_ = false;
    global_infos_.clear();
    global_objects_.clear();
    describe_.clear();
    session_.reset();
    client_.reset();
}

// --- salesforce_metadata_fields(catalog, object_name) ------------------------
//
// Read-only diagnostic: one row per field of `object_name` on the attached
// catalog, sourced through the shared engine (so it shares the metadata cache).
// Columns: object_name, field_name, type, filterable, sortable,
// relationship_name, reference_to LIST<VARCHAR>, picklist_values LIST<VARCHAR>
// (empty lists, never NULL, when a field has no targets / picklist values).

namespace {

struct MetaFieldRow {
    string field_name;
    string type;
    bool filterable;
    bool sortable;
    string relationship_name;
    vector<string> reference_to;
    vector<string> picklist_values;
};

struct MetaFieldsBindData : public FunctionData {
    string object_name;
    vector<MetaFieldRow> rows;
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<MetaFieldsBindData>(*this);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
};

struct MetaFieldsGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> MetaFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
        throw BinderException("salesforce_metadata_fields(catalog, object_name) requires a "
                              "non-NULL catalog alias and object name");
    }
    string alias = input.inputs[0].ToString();
    auto bind = make_uniq<MetaFieldsBindData>();
    bind->object_name = input.inputs[1].ToString();

    auto &eng = GetSalesforceCatalogMetadataEngine(context, alias);
    for (auto &fld : eng.GetObjectDescribe(context, bind->object_name).fields) {
        bind->rows.push_back({fld.name, fld.sf_type, fld.filterable, fld.sortable,
                              fld.relationship_name, fld.reference_to, fld.picklist_values});
    }

    names = {"object_name", "field_name",        "type",         "filterable",
             "sortable",    "relationship_name", "reference_to", "picklist_values"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::VARCHAR,
                    LogicalType::LIST(LogicalType::VARCHAR),
                    LogicalType::LIST(LogicalType::VARCHAR)};
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> MetaFieldsInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<MetaFieldsGlobalState>();
}

void MetaFieldsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<MetaFieldsBindData>();
    auto &gs = data.global_state->Cast<MetaFieldsGlobalState>();
    idx_t start = gs.cursor;
    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gs.cursor < bd.rows.size()) {
        const auto &f = bd.rows[gs.cursor];
        FlatVector::GetData<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], bd.object_name);
        FlatVector::GetData<string_t>(output.data[1])[row] =
            StringVector::AddString(output.data[1], f.field_name);
        FlatVector::GetData<string_t>(output.data[2])[row] =
            StringVector::AddString(output.data[2], f.type);
        FlatVector::GetData<bool>(output.data[3])[row] = f.filterable;
        FlatVector::GetData<bool>(output.data[4])[row] = f.sortable;
        if (f.relationship_name.empty()) {
            FlatVector::SetNull(output.data[5], row, true);
        } else {
            FlatVector::GetData<string_t>(output.data[5])[row] =
                StringVector::AddString(output.data[5], f.relationship_name);
        }
        gs.cursor++;
        row++;
    }

    // LIST<VARCHAR> columns: reference_to (col 6) and picklist_values (col 7).
    // A field with no values yields an empty list (length 0), never NULL.
    auto fill_list = [&](idx_t col, vector<string> MetaFieldRow::*member) {
        auto &lvec = output.data[col];
        idx_t total = 0;
        for (idx_t r = 0; r < row; r++) {
            total += (bd.rows[start + r].*member).size();
        }
        ListVector::Reserve(lvec, total);
        auto &child = ListVector::GetEntry(lvec);
        auto child_data = FlatVector::GetData<string_t>(child);
        auto entries = FlatVector::GetData<list_entry_t>(lvec);
        idx_t off = 0;
        for (idx_t r = 0; r < row; r++) {
            const auto &vals = bd.rows[start + r].*member;
            entries[r].offset = off;
            entries[r].length = vals.size();
            for (auto &v : vals) {
                child_data[off++] = StringVector::AddString(child, v);
            }
        }
        ListVector::SetListSize(lvec, off);
    };
    fill_list(6, &MetaFieldRow::reference_to);
    fill_list(7, &MetaFieldRow::picklist_values);

    output.SetCardinality(row);
}

// --- salesforce_metadata_objects(catalog) ------------------------------------
//
// Read-only diagnostic: one row per global sObject (object_name, queryable),
// sourced through the shared engine's global cache (one Describe Global, shared
// with GetGlobalObjects). Exposes non-queryable objects too (queryable=false).

struct MetaObjectsBindData : public FunctionData {
    vector<SalesforceObjectInfo> rows;
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<MetaObjectsBindData>(*this);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
};

struct MetaObjectsGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> MetaObjectsBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.empty() || input.inputs[0].IsNull()) {
        throw BinderException("salesforce_metadata_objects(catalog) requires a non-NULL "
                              "catalog alias");
    }
    string alias = input.inputs[0].ToString();
    auto bind = make_uniq<MetaObjectsBindData>();

    auto &eng = GetSalesforceCatalogMetadataEngine(context, alias);
    bind->rows = eng.GetGlobalObjectInfos(context);

    names = {"object_name", "queryable"};
    return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN};
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> MetaObjectsInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<MetaObjectsGlobalState>();
}

void MetaObjectsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<MetaObjectsBindData>();
    auto &gs = data.global_state->Cast<MetaObjectsGlobalState>();
    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gs.cursor < bd.rows.size()) {
        const auto &o = bd.rows[gs.cursor];
        FlatVector::GetData<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], o.name);
        FlatVector::GetData<bool>(output.data[1])[row] = o.queryable;
        gs.cursor++;
        row++;
    }
    output.SetCardinality(row);
}

// --- salesforce_query_explain() ----------------------------------------------
//
// Read-only, last-scan diagnostic. One row per projected field / conjunctive
// filter captured by the most recent catalog scan (DiagGetExplain), annotated
// via the shared Metadata Engine. Diagnostic-only: it reads the scan's
// write-only explain snapshot and never affects execution or query_cost().

struct ExplainRow {
    string object;
    string field;
    bool field_null = false;
    string role;
    bool resolved = false;
    bool resolved_null = false; // true for meta rows (count/transport)
    bool filterable = false;
    bool filterable_null = true;
    bool sortable = false;
    bool sortable_null = true;
    string relationship_name;
    bool relationship_null = true;
    vector<string> reference_to;
    bool pushed = false;
    bool residual = false;
    string reason;
    string guidance;
};

struct ExplainBindData : public FunctionData {
    vector<ExplainRow> rows;
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<ExplainBindData>(*this);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
};

struct ExplainGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static const char *ExplainGuidance(const string &reason) {
    if (reason == "pushed_to_soql") {
        return "filtered server-side via SOQL";
    }
    if (reason == "projected") {
        return "selected from Salesforce";
    }
    if (reason == "not_filterable") {
        return "field not filterable; DuckDB filters residually (over-fetch)";
    }
    if (reason == "unsupported_operator") {
        return "operator not expressible in SOQL; applied residually by DuckDB";
    }
    if (reason == "complex_expression") {
        return "complex predicate (OR/NOT/cross-field); applied residually by DuckDB";
    }
    if (reason == "unresolved_field") {
        return "field not found in object metadata";
    }
    if (reason == "relationship_traversed") {
        return "single-hop parent relationship expanded into the SOQL SELECT";
    }
    if (reason == "count_pushdown") {
        return "row count served by SELECT COUNT() — no records fetched";
    }
    if (reason == "count_not_pushed") {
        return "not a count-only scan; records are fetched normally";
    }
    return "metadata unavailable (catalog detached or engine error); annotation skipped";
}

unique_ptr<FunctionData> ExplainBind(ClientContext &context, TableFunctionBindInput &,
                                     vector<LogicalType> &return_types, vector<string> &names) {
    auto bind = make_uniq<ExplainBindData>();
    DiagExplainSnapshot snap = DiagGetExplain();

    // Resolve the owning catalog's describe through the shared engine. Any
    // failure (no alias, catalog detached, engine error) degrades every row to
    // metadata_unavailable — never throws.
    const SalesforceDescribe *desc = nullptr;
    bool meta_ok = false;
    if (!snap.catalog_alias.empty() && !snap.object.empty()) {
        try {
            auto &eng = GetSalesforceCatalogMetadataEngine(context, snap.catalog_alias);
            desc = &eng.GetObjectDescribe(context, snap.object);
            meta_ok = true;
        } catch (...) {
            meta_ok = false;
        }
    }

    for (auto &item : snap.items) {
        ExplainRow row;
        row.object = snap.object;
        row.pushed = item.pushed;
        row.residual = item.residual;

        // Relationship rows are self-contained (targets captured at scan time;
        // the synthesised parent STRUCT is not in the raw describe). Emit
        // directly — no engine needed, so unaffected by metadata availability.
        if (item.role == "relationship") {
            row.role = "relationship";
            row.field = item.field;
            row.field_null = item.field.empty();
            row.resolved = true; // the relationship was traversed
            if (!item.relationship_name.empty()) {
                row.relationship_name = item.relationship_name;
                row.relationship_null = false;
            }
            row.reference_to = item.reference_to;
            row.reason = "relationship_traversed";
            row.guidance = ExplainGuidance(row.reason);
            bind->rows.push_back(std::move(row));
            continue;
        }

        if (!meta_ok) {
            // Degrade: keep field text if known, but no metadata annotation.
            row.field = item.field;
            row.field_null = !item.field_known;
            row.role = item.role;
            row.resolved = false;
            row.reason = "metadata_unavailable";
            row.guidance = ExplainGuidance(row.reason);
            bind->rows.push_back(std::move(row));
            continue;
        }

        row.role = item.role;
        // Complex filter with no single field -> NULL field, complex_expression.
        if (!item.field_known) {
            row.field_null = true;
            row.resolved = false;
            row.reason = "complex_expression";
            row.guidance = ExplainGuidance(row.reason);
            bind->rows.push_back(std::move(row));
            continue;
        }

        row.field = item.field;
        const SalesforceField *fld = nullptr;
        for (auto &f : desc->fields) {
            if (StringUtil::CIEquals(f.name, item.field)) {
                fld = &f;
                break;
            }
        }
        if (!fld) {
            row.resolved = false;
            row.reason = "unresolved_field";
            row.guidance = ExplainGuidance(row.reason);
            bind->rows.push_back(std::move(row));
            continue;
        }

        row.resolved = true;
        row.filterable = fld->filterable;
        row.filterable_null = false;
        row.sortable = fld->sortable;
        row.sortable_null = false;
        if (!fld->relationship_name.empty()) {
            row.relationship_name = fld->relationship_name;
            row.relationship_null = false;
        }
        row.reference_to = fld->reference_to;

        if (item.role == "projection") {
            row.reason = "projected";
        } else if (item.pushed) {
            row.reason = "pushed_to_soql"; // exact-pushed or prefilter (also residual)
        } else if (!fld->filterable) {
            row.reason = "not_filterable";
        } else {
            row.reason = "unsupported_operator";
        }
        row.guidance = ExplainGuidance(row.reason);
        bind->rows.push_back(std::move(row));
    }

    // --- meta rows (synthesised from the ScanCost snapshot scalars) ----------
    // count: always one row reporting whether the scan used COUNT() pushdown.
    {
        ExplainRow c;
        c.object = snap.object;
        c.role = "count";
        c.field_null = true;
        c.resolved_null = true; // not a field resolution
        c.pushed = snap.count_pushdown;
        c.reason = snap.count_pushdown ? "count_pushdown" : "count_not_pushed";
        c.guidance = ExplainGuidance(c.reason);
        bind->rows.push_back(std::move(c));
    }
    // transport: always one row (rest|bulk) with reason/queryAll/est_rows detail.
    {
        ExplainRow t;
        t.object = snap.object;
        t.role = "transport";
        t.field_null = true;
        t.resolved_null = true;
        t.reason = (snap.transport == "bulk") ? "transport_bulk" : "transport_rest";
        string g = snap.transport_reason.empty() ? string("transport selected")
                                                  : snap.transport_reason;
        if (snap.query_mode == "queryAll") {
            g += "; queryAll (includes archived/deleted)";
        }
        if (snap.est_rows >= 0) {
            g += "; est_rows=" + std::to_string(snap.est_rows);
        }
        t.guidance = std::move(g);
        bind->rows.push_back(std::move(t));
    }

    names = {"object_name", "field_name",        "role",         "resolved",
             "filterable",  "sortable",          "relationship_name", "reference_to",
             "pushed",      "residual",          "reason",       "guidance"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
                    LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR),
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::VARCHAR,
                    LogicalType::VARCHAR};
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> ExplainInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<ExplainGlobalState>();
}

void ExplainFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<ExplainBindData>();
    auto &gs = data.global_state->Cast<ExplainGlobalState>();
    idx_t start = gs.cursor;
    idx_t row = 0;
    auto set_bool_or_null = [&](idx_t col, idx_t r, bool is_null, bool v) {
        if (is_null) {
            FlatVector::SetNull(output.data[col], r, true);
        } else {
            FlatVector::GetData<bool>(output.data[col])[r] = v;
        }
    };
    while (row < STANDARD_VECTOR_SIZE && gs.cursor < bd.rows.size()) {
        const auto &rw = bd.rows[gs.cursor];
        FlatVector::GetData<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], rw.object);
        if (rw.field_null) {
            FlatVector::SetNull(output.data[1], row, true);
        } else {
            FlatVector::GetData<string_t>(output.data[1])[row] =
                StringVector::AddString(output.data[1], rw.field);
        }
        FlatVector::GetData<string_t>(output.data[2])[row] =
            StringVector::AddString(output.data[2], rw.role);
        set_bool_or_null(3, row, rw.resolved_null, rw.resolved);
        set_bool_or_null(4, row, rw.filterable_null, rw.filterable);
        set_bool_or_null(5, row, rw.sortable_null, rw.sortable);
        if (rw.relationship_null) {
            FlatVector::SetNull(output.data[6], row, true);
        } else {
            FlatVector::GetData<string_t>(output.data[6])[row] =
                StringVector::AddString(output.data[6], rw.relationship_name);
        }
        FlatVector::GetData<bool>(output.data[8])[row] = rw.pushed;
        FlatVector::GetData<bool>(output.data[9])[row] = rw.residual;
        FlatVector::GetData<string_t>(output.data[10])[row] =
            StringVector::AddString(output.data[10], rw.reason);
        FlatVector::GetData<string_t>(output.data[11])[row] =
            StringVector::AddString(output.data[11], rw.guidance);
        gs.cursor++;
        row++;
    }

    // reference_to LIST<VARCHAR> (col 7): empty list when not a relationship.
    {
        auto &lvec = output.data[7];
        idx_t total = 0;
        for (idx_t r = 0; r < row; r++) {
            total += bd.rows[start + r].reference_to.size();
        }
        ListVector::Reserve(lvec, total);
        auto &child = ListVector::GetEntry(lvec);
        auto child_data = FlatVector::GetData<string_t>(child);
        auto entries = FlatVector::GetData<list_entry_t>(lvec);
        idx_t off = 0;
        for (idx_t r = 0; r < row; r++) {
            const auto &vals = bd.rows[start + r].reference_to;
            entries[r].offset = off;
            entries[r].length = vals.size();
            for (auto &v : vals) {
                child_data[off++] = StringVector::AddString(child, v);
            }
        }
        ListVector::SetListSize(lvec, off);
    }

    output.SetCardinality(row);
}

} // namespace

TableFunction GetSalesforceMetadataFieldsFunction() {
    return TableFunction("salesforce_metadata_fields",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR}, MetaFieldsFunction,
                         MetaFieldsBind, MetaFieldsInit);
}

TableFunction GetSalesforceMetadataObjectsFunction() {
    return TableFunction("salesforce_metadata_objects", {LogicalType::VARCHAR}, MetaObjectsFunction,
                         MetaObjectsBind, MetaObjectsInit);
}

TableFunction GetSalesforceQueryExplainFunction() {
    return TableFunction("salesforce_query_explain", {}, ExplainFunction, ExplainBind, ExplainInit);
}

} // namespace duckdb

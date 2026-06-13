// Metadata Engine v2 (ROADMAP v1.6 §17) — shared per-catalog read-only metadata
// cache. See salesforce_metadata_engine.hpp. Phase A: data accessors + cache +
// resolution helpers. The token/relationship resolution mirrors the Report
// Bridge rules; Report Bridge migrates onto this engine in Phase B (the
// duplicated helpers below are removed then).

#include "salesforce_metadata_engine.hpp"
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

const vector<string> &SalesforceMetadataEngine::GetGlobalObjects(ClientContext &context) {
    if (!global_loaded_) {
        IncGlobalDescribeCalls(); // DEBUG/TEST: proves Describe-Global-once
        global_objects_ = Session(context).GlobalDescribe();
        global_loaded_ = true;
    }
    return global_objects_;
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

} // namespace

TableFunction GetSalesforceMetadataFieldsFunction() {
    return TableFunction("salesforce_metadata_fields",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR}, MetaFieldsFunction,
                         MetaFieldsBind, MetaFieldsInit);
}

} // namespace duckdb

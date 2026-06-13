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

const vector<string> &SalesforceMetadataEngine::GetGlobalObjects(ClientContext &context) {
    if (!global_loaded_) {
        IncGlobalDescribeCalls(); // DEBUG/TEST: proves Describe-Global-once
        auto client = BuildHttpClientForContext(context);
        SalesforceSession session(config_, *client);
        session.SetToken(token_);
        global_objects_ = session.GlobalDescribe();
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
    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(config_, *client);
    session.SetToken(token_);
    auto res = describe_.emplace(key, session.Describe(object));
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
                                                   string &out_target) {
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
        return true;
    }
    return false;
}

void SalesforceMetadataEngine::Refresh(const string &object) {
    describe_.erase(StringUtil::Lower(object));
}

void SalesforceMetadataEngine::RefreshAll() {
    global_loaded_ = false;
    global_objects_.clear();
    describe_.clear();
}

// --- salesforce_metadata_probe(catalog, object) — TEST/foundation harness ----
//
// Exercises the engine (GetGlobalObjects + GetObjectDescribe + IsQueryable) so
// offline tests can assert cache hit / invalidation / per-catalog isolation via
// the salesforce_describe_calls() / salesforce_global_describe_calls() counters.
// Not a user-facing surface; the metadata diagnostic function is Phase C.

namespace {

struct MetaProbeBindData : public FunctionData {
    int64_t n_objects = 0;
    int64_t n_fields = 0;
    bool queryable = false;
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<MetaProbeBindData>(*this);
    }
    bool Equals(const FunctionData &) const override {
        return false;
    }
};

struct MetaProbeGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> MetaProbeBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
        throw BinderException("salesforce_metadata_probe(catalog, object) requires a "
                              "non-NULL catalog alias and object name");
    }
    string alias = input.inputs[0].ToString();
    string object = input.inputs[1].ToString();
    auto &eng = GetSalesforceCatalogMetadataEngine(context, alias);
    auto bind = make_uniq<MetaProbeBindData>();
    bind->n_objects = static_cast<int64_t>(eng.GetGlobalObjects(context).size());
    bind->n_fields = static_cast<int64_t>(eng.GetObjectDescribe(context, object).fields.size());
    bind->queryable = eng.IsQueryable(context, object);
    names = {"n_objects", "n_fields", "queryable"};
    return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BOOLEAN};
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> MetaProbeInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<MetaProbeGlobalState>();
}

void MetaProbeFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<MetaProbeBindData>();
    auto &gs = data.global_state->Cast<MetaProbeGlobalState>();
    if (gs.emitted) {
        output.SetCardinality(0);
        return;
    }
    FlatVector::GetData<int64_t>(output.data[0])[0] = bd.n_objects;
    FlatVector::GetData<int64_t>(output.data[1])[0] = bd.n_fields;
    FlatVector::GetData<bool>(output.data[2])[0] = bd.queryable;
    gs.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceMetadataProbeFunction() {
    return TableFunction("salesforce_metadata_probe",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR}, MetaProbeFunction,
                         MetaProbeBind, MetaProbeInit);
}

} // namespace duckdb

// Picklist / record-type metadata functions (#v1.3 §14). Read-only, lazy,
// cached per ATTACH. They parse the REST describe JSON (which already carries
// picklistValues per field and recordTypeInfos per object) — the scan schema
// drops these, so they are surfaced here as explicit table functions. No
// Metadata API, no SOAP, no Tooling: just the describe the catalog already has.

#include "salesforce_metadata.hpp"
#include "salesforce_storage.hpp"
#include "salesforce_json.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

struct MetaGlobalState : public GlobalTableFunctionState {
    bool built = false;
    vector<vector<Value>> rows;
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

string TrimArg(const Value &v) {
    string s = v.ToString();
    StringUtil::Trim(s);
    return s;
}

void CheckArgs(TableFunctionBindInput &input, idx_t n, const char *usage) {
    if (input.inputs.size() != n) {
        throw BinderException("%s", usage);
    }
    for (idx_t i = 0; i < n; i++) {
        if (input.inputs[i].IsNull()) {
            throw BinderException("salesforce metadata: argument %llu must not be NULL.",
                                  (unsigned long long)(i + 1));
        }
    }
}

// --- salesforce_picklist_values(catalog, object, field) --------------------

struct PicklistBindData : public TableFunctionData {
    string alias, object, field;
};

unique_ptr<FunctionData> PicklistBind(ClientContext &, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
    CheckArgs(input, 3,
              "salesforce_picklist_values(catalog, object, field) takes 3 arguments.");
    auto bind = make_uniq<PicklistBindData>();
    bind->alias = TrimArg(input.inputs[0]);
    bind->object = TrimArg(input.inputs[1]);
    bind->field = TrimArg(input.inputs[2]);
    names = {"value", "label", "active", "is_default"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
                    LogicalType::BOOLEAN};
    return std::move(bind);
}

void PicklistBuild(ClientContext &context, const PicklistBindData &bd, MetaGlobalState &gs) {
    string describe = GetSalesforceObjectDescribeJson(context, bd.alias, bd.object);
    // Find the field object within "fields" by name (case-insensitive).
    string field_obj;
    bool found = false;
    for (auto &f : sfjson::GetObjectArray(describe, "fields")) {
        if (StringUtil::CIEquals(sfjson::GetString(f, "name"), bd.field)) {
            field_obj = f;
            found = true;
            break;
        }
    }
    if (!found) {
        throw BinderException(
            "salesforce_picklist_values: field '%s' not found on object '%s'.", bd.field,
            bd.object);
    }
    // Non-picklist field -> no picklistValues -> 0 rows (not an error).
    for (auto &pv : sfjson::GetObjectArray(field_obj, "picklistValues")) {
        vector<Value> row;
        row.push_back(Value(sfjson::GetString(pv, "value")));
        row.push_back(Value(sfjson::GetString(pv, "label")));
        row.push_back(Value::BOOLEAN(sfjson::GetBool(pv, "active", true)));
        row.push_back(Value::BOOLEAN(sfjson::GetBool(pv, "defaultValue", false)));
        gs.rows.push_back(std::move(row));
    }
}

// --- salesforce_record_types(catalog, object) ------------------------------

struct RecordTypesBindData : public TableFunctionData {
    string alias, object;
};

unique_ptr<FunctionData> RecordTypesBind(ClientContext &, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types,
                                         vector<string> &names) {
    CheckArgs(input, 2, "salesforce_record_types(catalog, object) takes 2 arguments.");
    auto bind = make_uniq<RecordTypesBindData>();
    bind->alias = TrimArg(input.inputs[0]);
    bind->object = TrimArg(input.inputs[1]);
    names = {"developer_name", "label", "record_type_id", "active", "is_default"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BOOLEAN, LogicalType::BOOLEAN};
    return std::move(bind);
}

void RecordTypesBuild(ClientContext &context, const RecordTypesBindData &bd, MetaGlobalState &gs) {
    string describe = GetSalesforceObjectDescribeJson(context, bd.alias, bd.object);
    for (auto &rt : sfjson::GetObjectArray(describe, "recordTypeInfos")) {
        vector<Value> row;
        row.push_back(Value(sfjson::GetString(rt, "developerName")));
        row.push_back(Value(sfjson::GetString(rt, "name")));
        row.push_back(Value(sfjson::GetString(rt, "recordTypeId")));
        row.push_back(Value::BOOLEAN(sfjson::GetBool(rt, "active", true)));
        row.push_back(Value::BOOLEAN(sfjson::GetBool(rt, "defaultRecordTypeMapping", false)));
        gs.rows.push_back(std::move(row));
    }
}

unique_ptr<GlobalTableFunctionState> MetaInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<MetaGlobalState>();
}

template <class BindData, void (*Build)(ClientContext &, const BindData &, MetaGlobalState &)>
void MetaFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<BindData>();
    auto &gs = data.global_state->Cast<MetaGlobalState>();
    if (!gs.built) {
        Build(context, bd, gs);
        gs.built = true;
    }
    idx_t produced = 0;
    while (gs.cursor < gs.rows.size() && produced < STANDARD_VECTOR_SIZE) {
        auto &row = gs.rows[gs.cursor];
        for (idx_t c = 0; c < row.size(); c++) {
            output.data[c].SetValue(produced, row[c]);
        }
        gs.cursor++;
        produced++;
    }
    output.SetCardinality(produced);
}

} // namespace

TableFunction GetSalesforcePicklistValuesFunction() {
    return TableFunction("salesforce_picklist_values",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                         MetaFunction<PicklistBindData, PicklistBuild>, PicklistBind, MetaInit);
}

TableFunction GetSalesforceRecordTypesFunction() {
    return TableFunction("salesforce_record_types",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR},
                         MetaFunction<RecordTypesBindData, RecordTypesBuild>, RecordTypesBind,
                         MetaInit);
}

} // namespace duckdb

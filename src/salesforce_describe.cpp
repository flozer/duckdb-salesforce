// sObject describe -> schema (issue #5).
//
// ParseDescribe turns a describe JSON response into typed columns. The
// salesforce_describe(...) table function runs the authenticated describe at
// bind time (via SalesforceSession) and emits one row per field. No /query, no
// row scanning, no persisted cache — this only teaches the extension to
// describe a single object.

#include "salesforce_describe.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"
#include "salesforce_session.hpp"
#include "salesforce_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

SalesforceDescribe ParseDescribe(const string &json, const string &fallback_object) {
    SalesforceDescribe d;
    d.object_name = sfjson::GetString(json, "name");
    if (d.object_name.empty()) {
        d.object_name = fallback_object;
    }
    for (const auto &obj : sfjson::GetObjectArray(json, "fields")) {
        SalesforceField f;
        f.name = sfjson::GetString(obj, "name");
        if (f.name.empty()) {
            continue; // skip malformed entries
        }
        f.sf_type = sfjson::GetString(obj, "type");
        f.nillable = sfjson::GetBool(obj, "nillable", true);
        f.length = sfjson::GetInt(obj, "length", 0);
        f.precision = sfjson::GetInt(obj, "precision", 0);
        f.scale = sfjson::GetInt(obj, "scale", 0);
        f.filterable = sfjson::GetBool(obj, "filterable", false);
        f.sortable = sfjson::GetBool(obj, "sortable", false);
        bool unknown = false;
        f.duckdb_type = MapSalesforceType(f.sf_type, f.precision, f.scale, &unknown);
        f.unknown_type = unknown;
        d.fields.push_back(std::move(f));
    }
    return d;
}

namespace {

struct DescribeBindData : public FunctionData {
    SalesforceDescribe describe;

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<DescribeBindData>();
        r->describe = describe;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<DescribeBindData>();
        return describe.object_name == other.describe.object_name &&
               describe.fields.size() == other.describe.fields.size();
    }
};

struct DescribeGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static string NamedParam(TableFunctionBindInput &input, const char *key) {
    for (auto &kv : input.named_parameters) {
        if (StringUtil::CIEquals(kv.first, key)) {
            return kv.second.ToString();
        }
    }
    return "";
}

static unique_ptr<FunctionData> DescribeBind(ClientContext &context,
                                             TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types,
                                             vector<string> &names) {
    if (input.inputs.empty()) {
        throw BinderException(
            "salesforce_describe(object) requires the sObject name as the first argument");
    }
    string object = input.inputs[0].ToString();

    SalesforceConfig cfg;
    cfg.org = "describe";
    cfg.client_id = NamedParam(input, "client_id");
    cfg.client_secret = NamedParam(input, "client_secret");
    cfg.refresh_token = NamedParam(input, "refresh_token");
    string login_url = NamedParam(input, "login_url");
    string api_version = NamedParam(input, "api_version");
    cfg.login_url = login_url.empty() ? SalesforceConfig::kDefaultLoginUrl : login_url;
    cfg.api_version =
        api_version.empty() ? SalesforceConfig::kDefaultApiVersion : api_version;

    auto require = [&](const string &v, const char *key) {
        if (v.empty()) {
            throw BinderException(
                "salesforce_describe: missing required named parameter '%s'", key);
        }
    };
    require(cfg.client_id, "client_id");
    require(cfg.client_secret, "client_secret");
    require(cfg.refresh_token, "refresh_token");

    // Authenticate + describe at bind time (mock-injectable HTTP client).
    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(cfg, *client);
    session.Authenticate();

    auto bind = make_uniq<DescribeBindData>();
    bind->describe = session.Describe(object);

    names = {"name",  "sf_type",    "duckdb_type", "nullable",   "length",
             "precision", "scale",  "filterable",  "sortable",   "unknown_type"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::BOOLEAN, LogicalType::BIGINT,  LogicalType::BIGINT,
                    LogicalType::BIGINT,  LogicalType::BOOLEAN, LogicalType::BOOLEAN,
                    LogicalType::BOOLEAN};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> DescribeInitGlobal(ClientContext &,
                                                               TableFunctionInitInput &) {
    return make_uniq<DescribeGlobalState>();
}

static void DescribeFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<DescribeBindData>();
    auto &gstate = data.global_state->Cast<DescribeGlobalState>();
    const auto &fields = bind.describe.fields;

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gstate.cursor < fields.size()) {
        const auto &f = fields[gstate.cursor++];
        FlatVector::GetDataMutable<string_t>(output.data[0])[row] =
            StringVector::AddString(output.data[0], f.name);
        FlatVector::GetDataMutable<string_t>(output.data[1])[row] =
            StringVector::AddString(output.data[1], f.sf_type);
        FlatVector::GetDataMutable<string_t>(output.data[2])[row] =
            StringVector::AddString(output.data[2], f.duckdb_type.ToString());
        FlatVector::GetDataMutable<bool>(output.data[3])[row] = f.nillable;
        FlatVector::GetDataMutable<int64_t>(output.data[4])[row] = f.length;
        FlatVector::GetDataMutable<int64_t>(output.data[5])[row] = f.precision;
        FlatVector::GetDataMutable<int64_t>(output.data[6])[row] = f.scale;
        FlatVector::GetDataMutable<bool>(output.data[7])[row] = f.filterable;
        FlatVector::GetDataMutable<bool>(output.data[8])[row] = f.sortable;
        FlatVector::GetDataMutable<bool>(output.data[9])[row] = f.unknown_type;
        row++;
    }
    output.SetCardinality(row);
}

} // namespace

TableFunction GetSalesforceDescribeFunction() {
    TableFunction fn("salesforce_describe", {LogicalType::VARCHAR}, DescribeFunction,
                     DescribeBind, DescribeInitGlobal);
    fn.named_parameters["client_id"] = LogicalType::VARCHAR;
    fn.named_parameters["client_secret"] = LogicalType::VARCHAR;
    fn.named_parameters["refresh_token"] = LogicalType::VARCHAR;
    fn.named_parameters["login_url"] = LogicalType::VARCHAR;
    fn.named_parameters["api_version"] = LogicalType::VARCHAR;
    return fn;
}

} // namespace duckdb

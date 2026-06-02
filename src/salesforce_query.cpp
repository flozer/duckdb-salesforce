// salesforce_query(...) table function — paginated SOQL fetcher (issue #6).
//
// Runs the query at bind time via SalesforceSession (auth + queryMore
// pagination + 401 refresh) and emits one row per fetched record as raw JSON.
// No typed decoding, no table scan, no pushdown — those are #7/#8/#9.

#include "salesforce_query.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_url.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

namespace {

struct QueryBindData : public FunctionData {
    SalesforceQueryResult result;

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<QueryBindData>();
        r->result = result;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<QueryBindData>();
        return result.records.size() == other.result.records.size();
    }
};

struct QueryGlobalState : public GlobalTableFunctionState {
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

static unique_ptr<FunctionData> QueryBind(ClientContext &context,
                                          TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types,
                                          vector<string> &names) {
    if (input.inputs.empty()) {
        throw BinderException(
            "salesforce_query(soql) requires the SOQL string as the first argument");
    }
    string soql = input.inputs[0].ToString();

    SalesforceConfig cfg;
    cfg.org = "query";
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
                "salesforce_query: missing required named parameter '%s'", key);
        }
    };
    require(cfg.client_id, "client_id");
    require(cfg.client_secret, "client_secret");
    require(cfg.refresh_token, "refresh_token");

    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(cfg, *client);
    session.Authenticate();

    auto bind = make_uniq<QueryBindData>();
    bind->result = session.Query(soql);

    names = {"record_index", "record_json"};
    return_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> QueryInitGlobal(ClientContext &,
                                                            TableFunctionInitInput &) {
    return make_uniq<QueryGlobalState>();
}

static void QueryFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<QueryBindData>();
    auto &gstate = data.global_state->Cast<QueryGlobalState>();
    const auto &records = bind.result.records;

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gstate.cursor < records.size()) {
        idx_t idx = gstate.cursor;
        FlatVector::GetData<int64_t>(output.data[0])[row] = static_cast<int64_t>(idx);
        FlatVector::GetData<string_t>(output.data[1])[row] =
            StringVector::AddString(output.data[1], records[idx]);
        gstate.cursor++;
        row++;
    }
    output.SetCardinality(row);
}

} // namespace

static void UrlEncodeScalar(DataChunk &args, ExpressionState &, Vector &result) {
    UnaryExecutor::Execute<string_t, string_t>(
        args.data[0], result, args.size(), [&](string_t input) {
            return StringVector::AddString(result, UrlEncodeComponent(input.GetString()));
        });
}

ScalarFunction GetSalesforceUrlEncodeFunction() {
    return ScalarFunction("sf_url_encode", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
                          UrlEncodeScalar);
}

TableFunction GetSalesforceQueryFunction() {
    TableFunction fn("salesforce_query", {LogicalType::VARCHAR}, QueryFunction, QueryBind,
                     QueryInitGlobal);
    fn.named_parameters["client_id"] = LogicalType::VARCHAR;
    fn.named_parameters["client_secret"] = LogicalType::VARCHAR;
    fn.named_parameters["refresh_token"] = LogicalType::VARCHAR;
    fn.named_parameters["login_url"] = LogicalType::VARCHAR;
    fn.named_parameters["api_version"] = LogicalType::VARCHAR;
    return fn;
}

} // namespace duckdb

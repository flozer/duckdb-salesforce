// Report Bridge (ROADMAP §16) — Phase A foundation only.
//
// salesforce_report_fetch_raw(report_id, mode) is a TEST/foundation harness that
// drives SalesforceSession::RunReport / DescribeReport through the existing auth
// + mock HTTP path. It returns the raw analytics JSON body plus the parsed
// top-level `allData` flag, so offline tests can prove endpoint routing, GET
// method, intact delivery, basic parse, and clear HTTP errors. The user-facing
// report functions (reports / report / report_soql) are Phases B-D.

#include "salesforce_report.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

struct ReportFetchBindData : public FunctionData {
    string body;
    bool all_data = false;

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<ReportFetchBindData>();
        r->body = body;
        r->all_data = all_data;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<ReportFetchBindData>();
        return body == other.body && all_data == other.all_data;
    }
};

struct ReportFetchGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
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

static unique_ptr<FunctionData> ReportFetchBind(ClientContext &context,
                                                TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types,
                                                vector<string> &names) {
    if (input.inputs.size() < 2) {
        throw BinderException(
            "salesforce_report_fetch_raw(report_id, mode) requires a report id and "
            "mode ('run' | 'describe')");
    }
    string report_id = input.inputs[0].ToString();
    string mode = StringUtil::Lower(input.inputs[1].ToString());
    if (mode != "run" && mode != "describe") {
        throw BinderException(
            "salesforce_report_fetch_raw: mode must be 'run' or 'describe' (got '%s')", mode);
    }

    SalesforceConfig cfg;
    cfg.org = "report";
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
                "salesforce_report_fetch_raw: missing required named parameter '%s'", key);
        }
    };
    require(cfg.client_id, "client_id");
    require(cfg.client_secret, "client_secret");
    require(cfg.refresh_token, "refresh_token");

    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(cfg, *client);
    session.Authenticate();

    auto bind = make_uniq<ReportFetchBindData>();
    bind->body = (mode == "describe") ? session.DescribeReport(report_id)
                                      : session.RunReport(report_id);
    bind->all_data = sfjson::GetBool(bind->body, "allData", false);

    names = {"body", "all_data"};
    return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReportFetchInit(ClientContext &,
                                                            TableFunctionInitInput &) {
    return make_uniq<ReportFetchGlobalState>();
}

static void ReportFetchFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<ReportFetchBindData>();
    auto &gstate = data.global_state->Cast<ReportFetchGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    FlatVector::GetData<string_t>(output.data[0])[0] =
        StringVector::AddString(output.data[0], bind.body);
    FlatVector::GetData<bool>(output.data[1])[0] = bind.all_data;
    gstate.emitted = true;
    output.SetCardinality(1);
}

// --- salesforce_reports(alias): list report definitions (Phase B) ------------

// Fixed projection over the queryable Report sObject. Lists DEFINITIONS, not
// report data. Equivalent raw query: SELECT ... FROM sf.Report.
static const char *kReportsSoql =
    "SELECT Id, Name, DeveloperName, FolderName, Format FROM Report";

struct ReportsBindData : public FunctionData {
    SalesforceConfig config;
    SalesforceTokenSet token;

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<ReportsBindData>();
        r->config = config;
        r->token = token;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<ReportsBindData>();
        return config.org == other.config.org;
    }
};

struct ReportsGlobalState : public GlobalTableFunctionState {
    bool fetched = false;
    vector<string> records;
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> ReportsBind(ClientContext &context,
                                            TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types,
                                            vector<string> &names) {
    if (input.inputs.empty()) {
        throw BinderException(
            "salesforce_reports(catalog) requires the attached Salesforce catalog "
            "alias, e.g. salesforce_reports('sf')");
    }
    if (input.inputs[0].IsNull()) {
        throw BinderException("salesforce_reports: catalog alias must not be NULL.");
    }
    string alias = input.inputs[0].ToString();

    auto bind = make_uniq<ReportsBindData>();
    // Reuse the attached catalog's authenticated credentials (validates the
    // alias early; throws a clear error if it is not a Salesforce catalog).
    GetSalesforceCatalogCredentials(context, alias, bind->config, bind->token);

    names = {"Id", "Name", "DeveloperName", "FolderName", "Format"};
    return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
                    LogicalType::VARCHAR, LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReportsInit(ClientContext &,
                                                        TableFunctionInitInput &) {
    return make_uniq<ReportsGlobalState>();
}

static void ReportsFunction(ClientContext &context, TableFunctionInput &data,
                            DataChunk &output) {
    auto &bd = data.bind_data->Cast<ReportsBindData>();
    auto &gs = data.global_state->Cast<ReportsGlobalState>();

    if (!gs.fetched) {
        auto client = BuildHttpClientForContext(context);
        SalesforceSession session(bd.config, *client);
        session.SetToken(bd.token);
        SetLastSoql(kReportsSoql);
        SalesforceQueryResult res = session.Query(kReportsSoql);
        gs.records = std::move(res.records);
        gs.fetched = true;
    }

    static const char *kKeys[] = {"Id", "Name", "DeveloperName", "FolderName", "Format"};
    idx_t produced = 0;
    while (gs.cursor < gs.records.size() && produced < STANDARD_VECTOR_SIZE) {
        const string &record = gs.records[gs.cursor];
        for (idx_t c = 0; c < 5; c++) {
            string val;
            bool found = false, is_null = false;
            sfjson::GetValue(record, kKeys[c], val, found, is_null);
            if (!found || is_null) {
                FlatVector::SetNull(output.data[c], produced, true);
            } else {
                FlatVector::GetData<string_t>(output.data[c])[produced] =
                    StringVector::AddString(output.data[c], val);
            }
        }
        gs.cursor++;
        produced++;
    }
    output.SetCardinality(produced);
}

} // namespace

TableFunction GetSalesforceReportsFunction() {
    return TableFunction("salesforce_reports", {LogicalType::VARCHAR}, ReportsFunction,
                         ReportsBind, ReportsInit);
}

TableFunction GetSalesforceReportFetchRawFunction() {
    TableFunction fn("salesforce_report_fetch_raw", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                     ReportFetchFunction, ReportFetchBind, ReportFetchInit);
    fn.named_parameters["client_id"] = LogicalType::VARCHAR;
    fn.named_parameters["client_secret"] = LogicalType::VARCHAR;
    fn.named_parameters["refresh_token"] = LogicalType::VARCHAR;
    fn.named_parameters["login_url"] = LogicalType::VARCHAR;
    fn.named_parameters["api_version"] = LogicalType::VARCHAR;
    return fn;
}

} // namespace duckdb

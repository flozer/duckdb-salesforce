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

// --- salesforce_report(alias, report_id): tabular sample (Phase C) -----------

static constexpr int64_t kReportMaxRows = 2000;
static const char *kReportGuidance =
    "report result is a validation sample only (max 2000 rows, no pagination); "
    "scale via sf.<Object> scans, not this function";

struct ReportBindData : public FunctionData {
    idx_t data_cols = 0;                 // count of report (non-reserved) columns
    vector<vector<string>> rows;         // cell labels, in detailColumns order
    bool truncated = false;
    bool all_data = false;
    bool has_all_data = false;           // false -> __sf_report_all_data is NULL

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<ReportBindData>();
        r->data_cols = data_cols;
        r->rows = rows;
        r->truncated = truncated;
        r->all_data = all_data;
        r->has_all_data = has_all_data;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &o = other_p.Cast<ReportBindData>();
        return data_cols == o.data_cols && rows.size() == o.rows.size();
    }
};

struct ReportGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> ReportBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types,
                                           vector<string> &names) {
    auto &args = input.inputs;
    if (args.size() < 2) {
        throw BinderException(
            "salesforce_report(catalog, report_id) requires the attached catalog "
            "alias and a report id, e.g. salesforce_report('sf', '00O...')");
    }
    if (args[0].IsNull() || args[1].IsNull()) {
        throw BinderException("salesforce_report: arguments must not be NULL.");
    }
    string alias = args[0].ToString();
    string report_id = args[1].ToString();

    SalesforceConfig config;
    SalesforceTokenSet token;
    GetSalesforceCatalogCredentials(context, alias, config, token);

    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(config, *client);
    session.SetToken(token);
    string body = session.RunReport(report_id);

    // Tabular only: the synchronous tabular factMap lives under the "T!T" key.
    // Summary/matrix reports key their factMap by grouping (e.g. "0!T") and are
    // out of the first cut.
    string factmap = sfjson::ExtractObject(body, "factMap");
    string tt = sfjson::ExtractObject(factmap, "T!T");
    if (tt.empty()) {
        throw BinderException(
            "salesforce_report: only tabular reports are supported in this cut "
            "(summary/matrix unsupported); report '%s'",
            report_id);
    }

    auto bind = make_uniq<ReportBindData>();

    // Columns: detailColumns order, named by the extended-metadata label.
    string metadata = sfjson::ExtractObject(body, "reportMetadata");
    vector<string> detail_cols = sfjson::GetStringArray(metadata, "detailColumns");
    string ext = sfjson::ExtractObject(body, "reportExtendedMetadata");
    string col_info = sfjson::ExtractObject(ext, "detailColumnInfo");
    for (auto &api : detail_cols) {
        string info = sfjson::ExtractObject(col_info, api);
        string label = info.empty() ? "" : sfjson::GetString(info, "label");
        names.push_back(label.empty() ? api : label);
        return_types.push_back(LogicalType::VARCHAR);
    }
    bind->data_cols = detail_cols.size();

    // Rows: dataCells[*].label, positional with detailColumns.
    for (auto &row_json : sfjson::GetObjectArray(tt, "rows")) {
        vector<string> vals;
        for (auto &cell : sfjson::GetObjectArray(row_json, "dataCells")) {
            vals.push_back(sfjson::GetString(cell, "label"));
        }
        vals.resize(bind->data_cols); // pad short rows; ignore extra cells
        bind->rows.push_back(std::move(vals));
    }

    // Truncation: allData when present (else unknown -> all_data NULL, no claim).
    string raw;
    bool found = false, is_null = false;
    sfjson::GetValue(body, "allData", raw, found, is_null);
    bind->has_all_data = found && !is_null;
    bind->all_data = (raw == "true");
    bind->truncated = bind->has_all_data ? !bind->all_data : false;

    // Reserved diagnostic columns (prefix cannot collide with report fields).
    names.push_back("__sf_report_truncated");
    return_types.push_back(LogicalType::BOOLEAN);
    names.push_back("__sf_report_all_data");
    return_types.push_back(LogicalType::BOOLEAN);
    names.push_back("__sf_report_max_rows");
    return_types.push_back(LogicalType::BIGINT);
    names.push_back("__sf_report_guidance");
    return_types.push_back(LogicalType::VARCHAR);

    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReportInit(ClientContext &,
                                                       TableFunctionInitInput &) {
    return make_uniq<ReportGlobalState>();
}

static void ReportFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<ReportBindData>();
    auto &gs = data.global_state->Cast<ReportGlobalState>();
    const idx_t dc = bd.data_cols;

    idx_t produced = 0;
    while (gs.cursor < bd.rows.size() && produced < STANDARD_VECTOR_SIZE) {
        const auto &vals = bd.rows[gs.cursor];
        for (idx_t c = 0; c < dc; c++) {
            FlatVector::GetData<string_t>(output.data[c])[produced] =
                StringVector::AddString(output.data[c], c < vals.size() ? vals[c] : string());
        }
        FlatVector::GetData<bool>(output.data[dc + 0])[produced] = bd.truncated;
        if (bd.has_all_data) {
            FlatVector::GetData<bool>(output.data[dc + 1])[produced] = bd.all_data;
        } else {
            FlatVector::SetNull(output.data[dc + 1], produced, true);
        }
        FlatVector::GetData<int64_t>(output.data[dc + 2])[produced] = kReportMaxRows;
        FlatVector::GetData<string_t>(output.data[dc + 3])[produced] =
            StringVector::AddString(output.data[dc + 3], kReportGuidance);
        gs.cursor++;
        produced++;
    }
    output.SetCardinality(produced);
}

// --- salesforce_report_soql(alias, report_id): candidate SOQL (Phase D) ------

struct ReportFilter {
    string field;
    string op;    // raw Salesforce operator (e.g. greaterThan, contains)
    string value;
};

// Map a Salesforce report filter operator to a SOQL comparison. Returns false
// for operators outside the safe subset (caller marks the report untranslatable).
static bool SafeOperator(const string &op, string &soql_op, bool &is_like) {
    is_like = false;
    if (op == "equals") { soql_op = "="; return true; }
    if (op == "notEqual") { soql_op = "!="; return true; }
    if (op == "lessThan") { soql_op = "<"; return true; }
    if (op == "greaterThan") { soql_op = ">"; return true; }
    if (op == "contains") { soql_op = "LIKE"; is_like = true; return true; }
    return false;
}

struct ReportSoqlBindData : public FunctionData {
    string report_id;
    string report_name;
    string report_type;
    string base_object;
    vector<string> columns;
    vector<ReportFilter> filters;
    string soql;
    bool translatable = false;
    string caveats;

    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<ReportSoqlBindData>(*this);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &o = other_p.Cast<ReportSoqlBindData>();
        return report_id == o.report_id && soql == o.soql;
    }
};

struct ReportSoqlGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> ReportSoqlBind(ClientContext &context,
                                               TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types,
                                               vector<string> &names) {
    auto &args = input.inputs;
    if (args.size() < 2) {
        throw BinderException(
            "salesforce_report_soql(catalog, report_id) requires the attached "
            "catalog alias and a report id");
    }
    if (args[0].IsNull() || args[1].IsNull()) {
        throw BinderException("salesforce_report_soql: arguments must not be NULL.");
    }
    string alias = args[0].ToString();

    auto bind = make_uniq<ReportSoqlBindData>();
    bind->report_id = args[1].ToString();

    SalesforceConfig config;
    SalesforceTokenSet token;
    GetSalesforceCatalogCredentials(context, alias, config, token);
    auto client = BuildHttpClientForContext(context);
    SalesforceSession session(config, *client);
    session.SetToken(token);
    string body = session.DescribeReport(bind->report_id);

    // Structured ingredients first — these are reliable from the describe.
    string metadata = sfjson::ExtractObject(body, "reportMetadata");
    bind->report_name = sfjson::GetString(metadata, "name");
    string report_type_obj = sfjson::ExtractObject(metadata, "reportType");
    bind->report_type = sfjson::GetString(report_type_obj, "type");
    // Best-effort base object: the report type's API name. The user must verify
    // it against an actual sObject (caveated below).
    bind->base_object = bind->report_type;
    string format = sfjson::GetString(metadata, "reportFormat");
    bind->columns = sfjson::GetStringArray(metadata, "detailColumns");
    for (auto &f : sfjson::GetObjectArray(metadata, "reportFilters")) {
        ReportFilter rf;
        rf.field = sfjson::GetString(f, "column");
        rf.op = sfjson::GetString(f, "operator");
        rf.value = sfjson::GetString(f, "value");
        bind->filters.push_back(std::move(rf));
    }

    // Translatability: conservative. Only single-object TABULAR with safe filter
    // operators yields a candidate SOQL; everything else stays untranslatable
    // with an explicit reason. Candidate SOQL is never an equivalence contract.
    vector<string> caveats;
    bind->translatable = true;
    if (!StringUtil::CIEquals(format, "TABULAR")) {
        bind->translatable = false;
        caveats.push_back("report format '" + format +
                          "' is not translatable in this cut (summary/matrix and "
                          "grouped/aggregated reports are unsupported)");
    }
    if (bind->base_object.empty()) {
        bind->translatable = false;
        caveats.push_back("base object could not be derived from the report type");
    }
    vector<string> clauses;
    for (auto &f : bind->filters) {
        string soql_op;
        bool is_like = false;
        if (!SafeOperator(f.op, soql_op, is_like)) {
            bind->translatable = false;
            caveats.push_back("unsupported filter operator '" + f.op + "'");
            continue;
        }
        if (is_like) {
            clauses.push_back(f.field + " LIKE '%" + f.value + "%'");
        } else {
            clauses.push_back(f.field + " " + soql_op + " " + f.value);
        }
    }

    if (bind->translatable) {
        string sql = "SELECT " + StringUtil::Join(bind->columns, ", ") + " FROM " +
                     bind->base_object;
        if (!clauses.empty()) {
            // First cut joins filters with AND; reportBooleanFilter logic (e.g.
            // "1 AND 2", OR) is left for a later cut and caveated.
            sql += " WHERE " + StringUtil::Join(clauses, " AND ");
        }
        bind->soql = sql;
        caveats.push_back("candidate SOQL is best-effort and NOT an equivalence "
                          "contract: base object and column/field API names are "
                          "derived from report metadata — validate against a "
                          "salesforce_report() sample before scaling");
        string bool_filter = sfjson::GetString(metadata, "reportBooleanFilter");
        if (!bool_filter.empty()) {
            caveats.push_back("report boolean filter logic '" + bool_filter +
                              "' was simplified to AND; verify the combination");
        }
    }
    bind->caveats = StringUtil::Join(caveats, "; ");

    child_list_t<LogicalType> filter_struct;
    filter_struct.push_back({"field", LogicalType::VARCHAR});
    filter_struct.push_back({"op", LogicalType::VARCHAR});
    filter_struct.push_back({"value", LogicalType::VARCHAR});

    names = {"report_id",  "report_name", "report_type", "base_object", "columns",
             "filters",    "soql",        "translatable", "caveats"};
    return_types = {LogicalType::VARCHAR,
                    LogicalType::VARCHAR,
                    LogicalType::VARCHAR,
                    LogicalType::VARCHAR,
                    LogicalType::LIST(LogicalType::VARCHAR),
                    LogicalType::LIST(LogicalType::STRUCT(filter_struct)),
                    LogicalType::VARCHAR,
                    LogicalType::BOOLEAN,
                    LogicalType::VARCHAR};
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReportSoqlInit(ClientContext &,
                                                           TableFunctionInitInput &) {
    return make_uniq<ReportSoqlGlobalState>();
}

static void ReportSoqlFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<ReportSoqlBindData>();
    auto &gs = data.global_state->Cast<ReportSoqlGlobalState>();
    if (gs.emitted) {
        output.SetCardinality(0);
        return;
    }

    auto str = [&](idx_t col, const string &v) {
        FlatVector::GetData<string_t>(output.data[col])[0] =
            StringVector::AddString(output.data[col], v);
    };
    str(0, bd.report_id);
    str(1, bd.report_name);
    str(2, bd.report_type);
    str(3, bd.base_object);

    // columns: LIST<VARCHAR>
    {
        auto &lvec = output.data[4];
        idx_t n = bd.columns.size();
        ListVector::Reserve(lvec, n);
        auto &child = ListVector::GetEntry(lvec);
        auto child_data = FlatVector::GetData<string_t>(child);
        for (idx_t k = 0; k < n; k++) {
            child_data[k] = StringVector::AddString(child, bd.columns[k]);
        }
        ListVector::SetListSize(lvec, n);
        FlatVector::GetData<list_entry_t>(lvec)[0] = list_entry_t(0, n);
    }

    // filters: LIST<STRUCT(field, op, value)>
    {
        auto &lvec = output.data[5];
        idx_t m = bd.filters.size();
        ListVector::Reserve(lvec, m);
        auto &child = ListVector::GetEntry(lvec); // STRUCT vector
        auto &entries = StructVector::GetEntries(child);
        auto fld = FlatVector::GetData<string_t>(*entries[0]);
        auto op = FlatVector::GetData<string_t>(*entries[1]);
        auto val = FlatVector::GetData<string_t>(*entries[2]);
        for (idx_t k = 0; k < m; k++) {
            fld[k] = StringVector::AddString(*entries[0], bd.filters[k].field);
            op[k] = StringVector::AddString(*entries[1], bd.filters[k].op);
            val[k] = StringVector::AddString(*entries[2], bd.filters[k].value);
        }
        ListVector::SetListSize(lvec, m);
        FlatVector::GetData<list_entry_t>(lvec)[0] = list_entry_t(0, m);
    }

    if (bd.translatable) {
        str(6, bd.soql);
    } else {
        FlatVector::SetNull(output.data[6], 0, true);
    }
    FlatVector::GetData<bool>(output.data[7])[0] = bd.translatable;
    str(8, bd.caveats);

    gs.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceReportsFunction() {
    return TableFunction("salesforce_reports", {LogicalType::VARCHAR}, ReportsFunction,
                         ReportsBind, ReportsInit);
}

TableFunction GetSalesforceReportFunction() {
    return TableFunction("salesforce_report", {LogicalType::VARCHAR, LogicalType::VARCHAR},
                         ReportFunction, ReportBind, ReportInit);
}

TableFunction GetSalesforceReportSoqlFunction() {
    return TableFunction("salesforce_report_soql",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR}, ReportSoqlFunction,
                         ReportSoqlBind, ReportSoqlInit);
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

// Report Bridge (ROADMAP §16) — three opt-in, read-only table functions:
//   salesforce_reports(catalog)            list report definitions
//   salesforce_report(catalog, id)         tabular sample + reserved diagnostics
//   salesforce_report_soql(catalog, id)    structured ingredients + candidate SOQL
//
// All run over the attached catalog's credentials and the synchronous Reports &
// Dashboards REST API (SalesforceSession::RunReport / DescribeReport). Tabular
// reports only; candidate SOQL is best-effort, never an equivalence contract.

#include "salesforce_report.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <unordered_map>
#include <unordered_set>

namespace duckdb {

namespace {

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
    bool Equals(const FunctionData &) const override {
        // Conservative: this bind carries remote-fetched, bind-time data. Never
        // claim equality so DuckDB cannot reuse/cache across distinct binds.
        return false;
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
    bool Equals(const FunctionData &) const override {
        // Conservative: carries remote-fetched rows + diagnostics. Never equal.
        return false;
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
    auto reserved_prefix = [](const string &n) {
        return n.rfind("__sf_report_", 0) == 0;
    };
    std::unordered_set<string> used;
    for (auto &api : detail_cols) {
        string info = sfjson::ExtractObject(col_info, api);
        string label = info.empty() ? "" : sfjson::GetString(info, "label");
        string name = label.empty() ? api : label;
        // A report column must never shadow a reserved diagnostic name: fall back
        // to the API name (then a safe prefix if the API name is also reserved).
        if (reserved_prefix(name)) {
            name = api;
        }
        if (name.empty() || reserved_prefix(name)) {
            name = "col_" + api;
        }
        // Disambiguate duplicate column names (DuckDB rejects duplicates).
        string base = name;
        idx_t n = 2;
        while (used.count(name)) {
            name = base + "_" + std::to_string(n++);
        }
        used.insert(name);
        names.push_back(name);
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

// SOQL string literal: single-quote and escape ' and \ per SOQL rules.
static string SoqlStr(const string &s) {
    string out = "'";
    for (char c : s) {
        if (c == '\\' || c == '\'') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Bare numeric literal (int or decimal). Safe to emit unquoted in SOQL.
static bool IsNumericLiteral(const string &s) {
    if (s.empty()) {
        return false;
    }
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    bool digit = false, dot = false;
    for (; i < s.size(); i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            digit = true;
        } else if (s[i] == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digit;
}

// Date/datetime/boolean/null values are NOT auto-translated: SOQL date literals
// are unquoted and context-specific, and true/false/null are ambiguous between a
// typed literal and a text value. Treat as ambiguous -> untranslatable.
static bool IsAmbiguousLiteral(const string &v) {
    string l = StringUtil::Lower(v);
    if (l == "true" || l == "false" || l == "null") {
        return true;
    }
    // YYYY-MM-DD... (date or ISO datetime) — leading 4 digits then '-'.
    if (v.size() >= 8 && v[4] == '-' && v[0] >= '0' && v[0] <= '9' && v[1] >= '0' &&
        v[1] <= '9' && v[2] >= '0' && v[2] <= '9' && v[3] >= '0' && v[3] <= '9') {
        return true;
    }
    return false;
}

// Safe Salesforce identifier: dot-separated segments, each [A-Za-z][A-Za-z0-9_]*.
// Covers Account, Custom__c, Account.Name, Parent__r.Name. Rejects spaces,
// commas, parens, quotes, operators, SQL aliases, subqueries, "*".
static bool IsSafeIdentifier(const string &s) {
    if (s.empty()) {
        return false;
    }
    auto valid_seg = [](const string &seg) {
        if (seg.empty()) {
            return false;
        }
        char c0 = seg[0];
        if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) {
            return false;
        }
        for (char c : seg) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) {
                return false;
            }
        }
        return true;
    };
    string seg;
    for (char c : s) {
        if (c == '.') {
            if (!valid_seg(seg)) {
                return false;
            }
            seg.clear();
        } else {
            seg.push_back(c);
        }
    }
    return valid_seg(seg);
}

// reportBooleanFilter is translatable only when empty or a pure conjunction
// "N AND N AND ..." with each index in [1, nfilters]. OR / NOT / parentheses /
// out-of-range indices are not supported (joining with AND would change meaning).
static bool BooleanFilterAndOnly(const string &bf, idx_t nfilters) {
    if (bf.empty()) {
        return true;
    }
    vector<string> toks;
    string t;
    for (char c : bf) {
        if (c == ' ' || c == '\t') {
            if (!t.empty()) {
                toks.push_back(t);
                t.clear();
            }
        } else {
            t.push_back(c);
        }
    }
    if (!t.empty()) {
        toks.push_back(t);
    }
    if (toks.empty() || toks.size() % 2 == 0) {
        return false; // must be index (AND index)*  -> odd token count
    }
    for (idx_t i = 0; i < toks.size(); i++) {
        if (i % 2 == 0) {
            for (char c : toks[i]) {
                if (c < '0' || c > '9') {
                    return false; // not a bare index (rejects '(', '1)', etc.)
                }
            }
            int idx = 0;
            try {
                idx = std::stoi(toks[i]);
            } catch (...) {
                return false;
            }
            if (idx < 1 || static_cast<idx_t>(idx) > nfilters) {
                return false;
            }
        } else if (!StringUtil::CIEquals(toks[i], "AND")) {
            return false; // OR / NOT / anything else (case-insensitive AND)
        }
    }
    return true;
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
    bool Equals(const FunctionData &) const override {
        // Conservative: carries remote-fetched ingredients + candidate SOQL +
        // caveats. Never equal.
        return false;
    }
};

struct ReportSoqlGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

// Small, explicit, fixture-backed map for standard report types. Each target is
// still validated as queryable in Describe Global before use — NOT a broad/auto
// map. Returns "" for unknown types.
static string BuiltinReportTypeObject(const string &report_type) {
    static const std::pair<const char *, const char *> kMap[] = {
        {"ContactList", "Contact"},
        {"AccountList", "Account"},
        {"OpportunityList", "Opportunity"},
    };
    for (auto &m : kMap) {
        if (StringUtil::CIEquals(report_type, m.first)) {
            return m.second;
        }
    }
    return "";
}

// Small, fixture-backed map for common report column tokens whose API name is
// not recoverable by simple normalization. The mapped target must still exist on
// the sObject Describe. Returns "" for unknown tokens. Case-insensitive key.
static string BuiltinReportToken(const string &token) {
    static const std::pair<const char *, const char *> kMap[] = {
        {"ID", "Id"},          {"NAME", "Name"},        {"FIRST_NAME", "FirstName"},
        {"LAST_NAME", "LastName"}, {"EMAIL", "Email"},  {"PHONE", "Phone"},
        {"CREATED_DATE", "CreatedDate"},
        // LAST_UPDATE -> LastModifiedDate intentionally omitted: not fixture-backed.
        // Add only with a real report fixture proving the token (conservative map).
    };
    for (auto &m : kMap) {
        if (StringUtil::CIEquals(token, m.first)) {
            return m.second;
        }
    }
    return "";
}

// Normalize an UPPER_SNAKE report token to PascalCase (FIRST_NAME -> FirstName,
// CREATED_DATE -> CreatedDate, EMAIL -> Email). Each '_'-part: first char upper,
// rest lower. Used only as a candidate that must then exist on the Describe.
static string NormalizeSnakeToken(const string &token) {
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

// Weak fallback: the single dotted prefix shared by report column / filter
// tokens (e.g. "Account.Name" -> "Account"). Returns "" when there is no dotted
// token or when prefixes are mixed/ambiguous (more than one distinct prefix).
static string DominantColumnPrefix(const vector<string> &columns,
                                   const vector<ReportFilter> &filters) {
    std::unordered_set<string> prefixes;
    auto add = [&](const string &tok) {
        auto dot = tok.find('.');
        if (dot != string::npos && dot > 0) {
            prefixes.insert(tok.substr(0, dot));
        }
    };
    for (auto &c : columns) {
        add(c);
    }
    for (auto &f : filters) {
        add(f.field);
    }
    if (prefixes.size() != 1) {
        return ""; // none, or ambiguous mixture -> reject
    }
    return *prefixes.begin();
}

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
    // SOQL is not SQL: validate format, identifiers, literals, and filter logic.
    // Any ambiguity -> translatable=false + soql NULL + an explicit caveat.
    vector<string> caveats;
    bool wildcard_caveat = false;
    bind->translatable = true;

    if (!StringUtil::CIEquals(format, "TABULAR")) {
        bind->translatable = false;
        caveats.push_back("report format '" + format +
                          "' is not translatable in this cut (summary/matrix and "
                          "grouped/aggregated reports are unsupported)");
    }
    // Cross filters (semi-/anti-join) are out of this cut.
    if (!sfjson::GetObjectArray(metadata, "crossFilters").empty()) {
        bind->translatable = false;
        caveats.push_back("report has cross filters (semi-/anti-join) which are not "
                          "supported in this cut");
    }

    // Phase 1 base-object resolver: derive an sObject CANDIDATE from the report
    // type through an ordered set of safe sources, then accept the first one that
    // is QUERYABLE in Describe Global. Sources, high -> low priority:
    //   1. custom_entity_suffix    "CustomEntity$X" -> X
    //   2. builtin_report_type_map small standard-type map (ContactList->Contact)
    //   3. column_prefix           single dotted prefix of columns/filters (weak)
    // reportTypeMetadata is intentionally NOT a source yet (possible Phase 1.1).
    struct BaseCandidate {
        string obj;
        const char *by;
    };
    vector<BaseCandidate> candidates;
    const string &rt = bind->report_type;
    auto dollar = rt.rfind('$');
    if (dollar != string::npos) {
        candidates.push_back({rt.substr(dollar + 1), "custom_entity_suffix"});
    } else {
        string mapped = BuiltinReportTypeObject(rt);
        if (!mapped.empty()) {
            candidates.push_back({mapped, "builtin_report_type_map"});
        }
    }
    {
        string pfx = DominantColumnPrefix(bind->columns, bind->filters);
        if (!pfx.empty()) {
            candidates.push_back({pfx, "column_prefix"});
        }
    }
    // Best-effort structured ingredient: first safe candidate, else the raw type.
    bind->base_object = rt;
    for (auto &c : candidates) {
        if (IsSafeIdentifier(c.obj)) {
            bind->base_object = c.obj;
            break;
        }
    }

    // Validate against the org: accept the first candidate that EXISTS and is
    // QUERYABLE in Describe Global, then load its field set. Resolving the base
    // only removes the base block — every projected/filtered field is still
    // validated below. No silent partial SOQL.
    string base;
    std::unordered_map<string, bool> field_filterable; // lower(name) -> filterable
    std::unordered_map<string, string> field_realname; // lower(name) -> real API name
    bool object_validated = false;
    if (bind->translatable) {
        if (candidates.empty()) {
            bind->translatable = false;
            caveats.push_back("could not derive a base object from report type '" + rt +
                              "'");
        } else {
            try {
                auto queryable = session.GlobalDescribe(); // one call, cached below
                const char *by = nullptr;
                for (auto &c : candidates) {
                    if (!IsSafeIdentifier(c.obj)) {
                        continue;
                    }
                    for (auto &n : queryable) {
                        if (StringUtil::CIEquals(n, c.obj)) {
                            base = c.obj;
                            by = c.by;
                            break;
                        }
                    }
                    if (!base.empty()) {
                        break; // first source that yields a queryable object wins
                    }
                }
                if (base.empty()) {
                    bind->translatable = false;
                    caveats.push_back("base object could not be resolved to a queryable "
                                      "object in Describe Global (report type '" + rt +
                                      "')");
                } else {
                    bind->base_object = base;
                    string by_s = by;
                    string label = (by_s == "custom_entity_suffix")
                                       ? "CustomEntity suffix (custom_entity_suffix)"
                                   : (by_s == "builtin_report_type_map")
                                       ? "builtin report type map (builtin_report_type_map)"
                                       : "column prefix (column_prefix, low confidence)";
                    caveats.push_back("base object resolved from " + label + ": '" + rt +
                                      "' -> '" + base + "'");
                    for (auto &fld : session.Describe(base).fields) {
                        string key = StringUtil::Lower(fld.name);
                        field_filterable[key] = fld.filterable;
                        field_realname[key] = fld.name;
                    }
                    object_validated = true;
                    // column_prefix is a diagnostic hint only: a shared "Account."
                    // prefix could be a relationship on a different root object
                    // (e.g. Contact with Account.Name -> FROM Contact, not Account).
                    // It can fill base_object/provenance but must NOT enable
                    // translatable — better an honest false than a wrong root.
                    if (by_s == "column_prefix") {
                        bind->translatable = false;
                        caveats.push_back("base object inferred from column prefix only; "
                                          "not enough to guarantee the report root object");
                    }
                }
            } catch (...) {
                bind->translatable = false;
                caveats.push_back("could not validate the base object against the org "
                                  "describe");
            }
        }
    }

    // Resolve a report column/filter token to a DIRECT field of the base object:
    // strip the "<base>." prefix, require a bare safe identifier that exists on
    // the sObject. Relationship traversal (Rel.Field) and pseudo columns do not
    // resolve -> translatable=false.
    // Resolve a report token to a REAL field API name on the base sObject.
    // Phase 2: token -> field via as-is (case-insensitive), small builtin token
    // map, or UPPER_SNAKE->PascalCase normalization — each candidate must EXIST
    // on the Describe (out = the real API name). A dotted token only resolves
    // when its prefix IS the base object (same-base); any other prefix is a
    // relationship -> unresolved (Phase 3). Never guesses a name not in Describe.
    auto resolve_field = [&](const string &token, string &out) -> bool {
        string f = token;
        auto dot = f.find('.');
        if (dot != string::npos) {
            if (!StringUtil::CIEquals(f.substr(0, dot), base)) {
                return false; // relationship traversal -> Phase 3
            }
            f = f.substr(dot + 1);
            if (f.empty() || f.find('.') != string::npos) {
                return false; // multi-hop relationship
            }
        }
        if (f.empty()) {
            return false;
        }
        vector<string> cands;
        cands.push_back(f); // as-is (matched case-insensitively below)
        string mapped = BuiltinReportToken(f);
        if (!mapped.empty()) {
            cands.push_back(mapped);
        }
        cands.push_back(NormalizeSnakeToken(f));
        for (auto &c : cands) {
            auto it = field_realname.find(StringUtil::Lower(c));
            if (it != field_realname.end()) {
                out = it->second; // real API name from the Describe
                return true;
            }
        }
        return false;
    };

    vector<string> soql_fields;
    if (object_validated) {
        for (auto &c : bind->columns) {
            string field;
            if (!resolve_field(c, field)) {
                bind->translatable = false;
                caveats.push_back("column '" + c + "' does not resolve to a field on '" +
                                  base + "'");
                continue;
            }
            soql_fields.push_back(field);
        }
        if (soql_fields.empty()) {
            bind->translatable = false;
            caveats.push_back("no projectable columns resolve on '" + base + "'");
        }
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
        if (!object_validated) {
            continue; // object not validated -> already untranslatable
        }
        string field;
        if (!resolve_field(f.field, field)) {
            bind->translatable = false;
            caveats.push_back("filter field '" + f.field +
                              "' does not resolve to a field on '" + base + "'");
            continue;
        }
        // A WHERE field must be filterable, or the candidate SOQL would fail at
        // Salesforce. (Projection only needs the field to exist.)
        if (!field_filterable[StringUtil::Lower(field)]) {
            bind->translatable = false;
            caveats.push_back("filter field '" + f.field + "' is not filterable on '" +
                              base + "'");
            continue;
        }
        if (is_like) {
            // contains -> LIKE '%v%' (always a quoted string). Broad wildcard.
            clauses.push_back(field + " LIKE " + SoqlStr("%" + f.value + "%"));
            wildcard_caveat = true;
        } else if (IsNumericLiteral(f.value)) {
            clauses.push_back(field + " " + soql_op + " " + f.value);
        } else if (IsAmbiguousLiteral(f.value)) {
            bind->translatable = false;
            caveats.push_back("filter value '" + f.value +
                              "' (date/boolean/null) is not auto-translated; write "
                              "the SOQL literal manually");
        } else {
            clauses.push_back(field + " " + soql_op + " " + SoqlStr(f.value));
        }
    }

    string bool_filter = sfjson::GetString(metadata, "reportBooleanFilter");
    if (!BooleanFilterAndOnly(bool_filter, bind->filters.size())) {
        bind->translatable = false;
        caveats.push_back("report filter logic '" + bool_filter +
                          "' uses OR/NOT/grouping (or an out-of-range index) that "
                          "is not supported in this cut");
    }

    if (wildcard_caveat) {
        caveats.push_back("contains was mapped to a broad LIKE '%...%' wildcard, "
                          "which can be slow/non-selective on large objects");
    }

    if (bind->translatable) {
        string sql = "SELECT " + StringUtil::Join(soql_fields, ", ") + " FROM " + base;
        if (!clauses.empty()) {
            sql += " WHERE " + StringUtil::Join(clauses, " AND ");
        }
        bind->soql = sql;
        caveats.push_back("candidate SOQL is best-effort and NOT an equivalence "
                          "contract: base object and field API names are derived "
                          "from report metadata — validate against a "
                          "salesforce_report() sample before scaling");
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

} // namespace duckdb

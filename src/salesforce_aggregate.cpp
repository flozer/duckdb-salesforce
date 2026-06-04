// salesforce_aggregate(catalog, object, aggregates [, filter]) — explicit,
// opt-in server-side SOQL aggregates (#v1.0). A plain table function: no
// optimizer, no plan rewrite. It reuses an already-ATTACHed catalog's
// authenticated session (no re-auth, no secrets in the call), runs
//   SELECT <aggregates> FROM <object> [WHERE <filter>]
// and returns ONE row with one VARCHAR column per aggregate term. Honors
// sf_query_mode (query / queryAll). Records the SOQL in the diagnostics.

#include "salesforce_aggregate.hpp"
#include "salesforce_storage.hpp"
#include "salesforce_session.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_config.hpp"
#include "salesforce_auth.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

namespace {

constexpr idx_t kMaxArgLen = 4000; // safe cap on user-supplied SOQL fragments

// Allowed SOQL aggregate functions (upper-cased). A term MUST start with one of
// these — bare fields are rejected so the single-row contract holds and a
// "SELECT field" that drags every row down cannot slip through.
bool IsAggregateFn(const string &fn_upper) {
    return fn_upper == "COUNT" || fn_upper == "COUNT_DISTINCT" || fn_upper == "SUM" ||
           fn_upper == "AVG" || fn_upper == "MIN" || fn_upper == "MAX";
}

bool IsIdentifier(const string &s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return false;
    }
    for (char c : s) {
        if (!(std::isalnum((unsigned char)c) || c == '_')) {
            return false;
        }
    }
    return true;
}

string Trim(const string &s) {
    string t = s;
    StringUtil::Trim(t);
    return t;
}

// Reject the things that have no place in a single read-only aggregate query:
// statement separators and nested SELECTs (belt-and-suspenders — SOQL has no
// statement separator, but this blocks accidental subqueries this cut won't
// validate). Never echoes secrets (these are the user's own SOQL).
void RejectUnsafe(const string &what, const string &v) {
    if (v.size() > kMaxArgLen) {
        throw BinderException("salesforce_aggregate: %s is too long (max %llu chars).", what,
                              (unsigned long long)kMaxArgLen);
    }
    if (v.find(';') != string::npos) {
        throw BinderException("salesforce_aggregate: %s must not contain ';'.", what);
    }
    if (StringUtil::Contains(StringUtil::Upper(v), "SELECT")) {
        throw BinderException(
            "salesforce_aggregate: %s must not contain a nested SELECT.", what);
    }
}

// Split on top-level commas (commas inside parentheses belong to a function's
// argument list and are kept).
vector<string> SplitTopLevelCommas(const string &s) {
    vector<string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth > 0) {
                depth--;
            }
        } else if (c == ',' && depth == 0) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(s.substr(start));
    return out;
}

struct AggColumn {
    string name; // output column name (alias, else exprN)
    string key;  // JSON key Salesforce returns (same as name)
};

// Parse one aggregate term ("MIN(Field) alias" / "COUNT(Id)") -> column name +
// JSON key. `unaliased` is the running count of alias-less terms (for exprN).
AggColumn ParseTerm(const string &term_raw, idx_t &unaliased) {
    string term = Trim(term_raw);
    if (term.empty()) {
        throw BinderException("salesforce_aggregate: empty aggregate term.");
    }
    auto lp = term.find('(');
    if (lp == string::npos) {
        throw BinderException(
            "salesforce_aggregate: '%s' is not an aggregate — each term must be "
            "MIN/MAX/SUM/AVG/COUNT/COUNT_DISTINCT(field).",
            term);
    }
    string fn = StringUtil::Upper(Trim(term.substr(0, lp)));
    if (!IsAggregateFn(fn)) {
        throw BinderException(
            "salesforce_aggregate: '%s' is not a supported aggregate function "
            "(use MIN, MAX, SUM, AVG, COUNT, or COUNT_DISTINCT).",
            fn);
    }
    // Match the balanced parentheses of the call.
    int depth = 0;
    size_t close = string::npos;
    for (size_t i = lp; i < term.size(); i++) {
        if (term[i] == '(') {
            depth++;
        } else if (term[i] == ')') {
            if (--depth == 0) {
                close = i;
                break;
            }
        }
    }
    if (close == string::npos) {
        throw BinderException("salesforce_aggregate: unbalanced parentheses in '%s'.", term);
    }
    string alias = Trim(term.substr(close + 1));
    // Tolerate an optional leading "AS " before the alias.
    if (alias.size() >= 3 && StringUtil::Upper(alias.substr(0, 3)) == "AS " ) {
        alias = Trim(alias.substr(3));
    }
    AggColumn col;
    if (alias.empty()) {
        col.name = "expr" + std::to_string(unaliased++);
        col.key = col.name;
    } else {
        if (!IsIdentifier(alias)) {
            throw BinderException(
                "salesforce_aggregate: alias '%s' is not a valid identifier.", alias);
        }
        col.name = alias;
        col.key = alias;
    }
    return col;
}

struct AggBindData : public TableFunctionData {
    string soql;
    vector<AggColumn> columns;
    SalesforceConfig config;
    SalesforceTokenSet token;
};

struct AggGlobalState : public GlobalTableFunctionState {
    bool fetched = false;
    vector<string> records; // raw JSON record objects (one per group row)
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> AggBind(ClientContext &context, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, vector<string> &names) {
    auto &args = input.inputs;
    if (args.size() < 3 || args.size() > 5) {
        throw BinderException(
            "salesforce_aggregate(catalog, object, aggregates [, filter [, "
            "group_by]]) takes 3 to 5 arguments.");
    }
    for (idx_t i = 0; i < args.size(); i++) {
        if (args[i].IsNull()) {
            throw BinderException("salesforce_aggregate: argument %llu must not be NULL.",
                                  (unsigned long long)(i + 1));
        }
    }
    string alias = Trim(args[0].ToString());
    string object = Trim(args[1].ToString());
    string aggregates = Trim(args[2].ToString());
    string filter = args.size() >= 4 ? Trim(args[3].ToString()) : string();
    string group_by = args.size() == 5 ? Trim(args[4].ToString()) : string();

    if (!IsIdentifier(object)) {
        throw BinderException(
            "salesforce_aggregate: object '%s' is not a valid sObject name.", object);
    }
    if (aggregates.empty()) {
        throw BinderException("salesforce_aggregate: 'aggregates' must not be empty.");
    }
    RejectUnsafe("aggregates", aggregates);
    if (!filter.empty()) {
        RejectUnsafe("filter", filter);
    }

    // group_by: simple field identifiers only (no dotted fields, expressions,
    // ROLLUP/CUBE — those fail the identifier check). Group columns come FIRST.
    vector<string> group_fields;
    if (!group_by.empty()) {
        RejectUnsafe("group_by", group_by);
        for (auto &g : SplitTopLevelCommas(group_by)) {
            string field = Trim(g);
            if (!IsIdentifier(field)) {
                throw BinderException(
                    "salesforce_aggregate: group_by '%s' must be a simple field "
                    "identifier (no dotted fields, functions, ROLLUP/CUBE/HAVING).",
                    field);
            }
            group_fields.push_back(field);
        }
    }

    auto bind = make_uniq<AggBindData>();
    // Group columns first, named by field; the JSON key is the field name.
    for (auto &field : group_fields) {
        AggColumn col;
        col.name = field;
        col.key = field;
        names.push_back(col.name);
        return_types.push_back(LogicalType::VARCHAR);
        bind->columns.push_back(std::move(col));
    }
    idx_t unaliased = 0;
    for (auto &term : SplitTopLevelCommas(aggregates)) {
        AggColumn col = ParseTerm(term, unaliased);
        names.push_back(col.name);
        return_types.push_back(LogicalType::VARCHAR);
        bind->columns.push_back(std::move(col));
    }

    string group_clause = StringUtil::Join(group_fields, ", ");
    bind->soql = "SELECT ";
    if (!group_fields.empty()) {
        bind->soql += group_clause + ", ";
    }
    bind->soql += aggregates + " FROM " + object;
    if (!filter.empty()) {
        bind->soql += " WHERE " + filter;
    }
    if (!group_fields.empty()) {
        bind->soql += " GROUP BY " + group_clause;
    }

    // Reuse the attached catalog's authenticated credentials (validates the
    // alias early; throws a clear error if it is not a Salesforce catalog).
    GetSalesforceCatalogCredentials(context, alias, bind->config, bind->token);
    return std::move(bind);
}

unique_ptr<GlobalTableFunctionState> AggInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<AggGlobalState>();
}

void AggFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
    auto &bd = data.bind_data->Cast<AggBindData>();
    auto &gs = data.global_state->Cast<AggGlobalState>();

    // Fetch once: run the SOQL and keep every group row. GROUP BY can return
    // many rows, so they are emitted across calls via the cursor below.
    if (!gs.fetched) {
        auto client = BuildHttpClientForContext(context);
        SalesforceSession session(bd.config, *client);
        session.SetToken(bd.token);

        bool query_all = false;
        Value qm;
        if (context.TryGetCurrentSetting("sf_query_mode", qm) && !qm.IsNull()) {
            query_all = StringUtil::Lower(qm.ToString()) == "queryall";
        }
        session.SetQueryAll(query_all);

        SetLastSoql(bd.soql);
        SetLastTransport("rest", -1,
                         query_all ? "explicit aggregate (queryAll)" : "explicit aggregate");

        SalesforceQueryResult res = session.Query(bd.soql);
        gs.records = std::move(res.records);
        gs.fetched = true;
    }

    idx_t produced = 0;
    while (gs.cursor < gs.records.size() && produced < STANDARD_VECTOR_SIZE) {
        const string &record = gs.records[gs.cursor];
        for (idx_t c = 0; c < bd.columns.size(); c++) {
            string val;
            bool found = false, is_null = false;
            sfjson::GetValue(record, bd.columns[c].key, val, found, is_null);
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

TableFunction GetSalesforceAggregateFunction() {
    TableFunction fn("salesforce_aggregate",
                     {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                     AggFunction, AggBind, AggInit);
    fn.varargs = LogicalType::VARCHAR; // optional 4th arg: filter
    return fn;
}

} // namespace duckdb

// SOQL generation + filter-pushdown translation (issue #9).
//
// Conservative + residual-safe: only a small, well-understood subset of filters
// is translated to a SOQL WHERE. Anything else (OR, IN, LIKE, expressions,
// non-filterable fields, over-long WHERE) is simply omitted — DuckDB's table
// scan still applies the full filter set to the returned rows, so results are
// always correct; pushdown is purely an over-fetch optimisation.

#include "salesforce_soql.hpp"
#include "salesforce_describe.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <mutex>

namespace duckdb {

// Max generated WHERE length before we give up and residualise (Appendix A).
static constexpr size_t kMaxWhereChars = 4000;

// Single-quote a string as a SOQL literal, escaping backslash and quote.
static string SoqlString(const string &s) {
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

string SoqlLiteral(const Value &value) {
    if (value.IsNull()) {
        return "null";
    }
    switch (value.type().id()) {
    case LogicalTypeId::VARCHAR:
        return SoqlString(StringValue::Get(value));
    case LogicalTypeId::BOOLEAN:
        return value.GetValue<bool>() ? "true" : "false";
    case LogicalTypeId::TINYINT:
    case LogicalTypeId::SMALLINT:
    case LogicalTypeId::INTEGER:
    case LogicalTypeId::BIGINT:
    case LogicalTypeId::UTINYINT:
    case LogicalTypeId::USMALLINT:
    case LogicalTypeId::UINTEGER:
    case LogicalTypeId::UBIGINT:
    case LogicalTypeId::FLOAT:
    case LogicalTypeId::DOUBLE:
    case LogicalTypeId::DECIMAL:
        return value.ToString(); // numeric literals are bare in SOQL
    case LogicalTypeId::DATE:
        return value.ToString(); // yyyy-MM-dd
    case LogicalTypeId::TIMESTAMP: {
        // DuckDB "yyyy-MM-dd HH:mm:ss[.fff]" -> SOQL ISO8601 "...THH:mm:ssZ".
        string s = value.ToString();
        auto sp = s.find(' ');
        if (sp != string::npos) {
            s[sp] = 'T';
        }
        return s + "Z";
    }
    case LogicalTypeId::TIME:
        return value.ToString() + "Z";
    default:
        throw NotImplementedException("unsupported SOQL literal type");
    }
}

static bool ComparisonOp(ExpressionType type, string &op) {
    switch (type) {
    case ExpressionType::COMPARE_EQUAL: op = "="; return true;
    case ExpressionType::COMPARE_NOTEQUAL: op = "!="; return true;
    case ExpressionType::COMPARE_LESSTHAN: op = "<"; return true;
    case ExpressionType::COMPARE_GREATERTHAN: op = ">"; return true;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO: op = "<="; return true;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: op = ">="; return true;
    default: return false;
    }
}

// Translate one filter on `field` into SOQL. Returns false (=> residual) for
// anything outside the safe subset. Never throws — a failed literal/op is
// reported as not-translatable.
// Flip a comparison operator (for `constant <op> column` -> `column <op'> ...`).
static bool FlipComparison(ExpressionType in, ExpressionType &out) {
    switch (in) {
    case ExpressionType::COMPARE_EQUAL: out = in; return true;
    case ExpressionType::COMPARE_NOTEQUAL: out = in; return true;
    case ExpressionType::COMPARE_LESSTHAN: out = ExpressionType::COMPARE_GREATERTHAN; return true;
    case ExpressionType::COMPARE_GREATERTHAN: out = ExpressionType::COMPARE_LESSTHAN; return true;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO: out = ExpressionType::COMPARE_GREATERTHANOREQUALTO; return true;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO: out = ExpressionType::COMPARE_LESSTHANOREQUALTO; return true;
    default: return false;
    }
}

// Resolve a column-ref expression (projection-relative index) to a filterable
// SalesforceField name via the projection->field map.
static bool FieldFor(const Expression &expr, const vector<SalesforceField> &fields,
                     const vector<idx_t> &projection_to_field, string &name) {
    if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
        return false;
    }
    auto &col = expr.Cast<BoundColumnRefExpression>();
    idx_t proj = col.binding.column_index;
    if (proj >= projection_to_field.size()) {
        return false;
    }
    idx_t idx = projection_to_field[proj];
    if (idx >= fields.size() || !fields[idx].filterable) {
        return false;
    }
    name = fields[idx].name;
    return true;
}

// Defensive cap: a huge IN list bloats the WHERE and isn't worth pushing.
static constexpr idx_t kMaxInItems = 200;

// Translate one conjunct into a SOQL predicate. Returns false (=> residual) for
// anything outside the safe subset. Sets `exact` true only when the SOQL clause
// matches DuckDB semantics exactly (safe to remove from the residual filter set);
// false when it is a SUPERSET prefilter (IN/LIKE/OR) that MUST stay residual so
// DuckDB refines the result. Never throws.
static bool TranslateExpr(const Expression &expr, const vector<SalesforceField> &fields,
                          const vector<idx_t> &projection_to_field, string &out, bool &exact) {
    exact = true;
    try {
        auto klass = expr.GetExpressionClass();
        auto etype = expr.GetExpressionType();

        // column <cmp> constant (exact).
        if (klass == ExpressionClass::BOUND_COMPARISON) {
            auto &cmp_expr = expr.Cast<BoundComparisonExpression>();
            const Expression &l = *cmp_expr.left;
            const Expression &r = *cmp_expr.right;
            string field;
            ExpressionType cmp = etype;
            const Expression *constant = nullptr;
            if (FieldFor(l, fields, projection_to_field, field) &&
                r.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                constant = &r;
            } else if (FieldFor(r, fields, projection_to_field, field) &&
                       l.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                if (!FlipComparison(etype, cmp)) {
                    return false;
                }
                constant = &l;
            } else {
                return false;
            }
            string op;
            if (!ComparisonOp(cmp, op)) {
                return false;
            }
            out = field + " " + op + " " +
                  SoqlLiteral(constant->Cast<BoundConstantExpression>().value);
            return true;
        }

        // column BETWEEN lower AND upper (exact). DuckDB's FilterCombiner
        // rewrites `field >= lo AND field < hi` (and the >/<= operand/inclusivity
        // variants, including reversed `const <op> field`) on ONE column into a
        // single BoundBetweenExpression before it reaches pushdown. Without this
        // case the whole range fell back to residual (full remote scan). Emit the
        // two half-ranges with inclusivity-correct operators; exact because both
        // bounds are plain column-vs-constant comparisons matching SOQL.
        if (klass == ExpressionClass::BOUND_BETWEEN) {
            auto &bw = expr.Cast<BoundBetweenExpression>();
            string field;
            if (!FieldFor(*bw.input, fields, projection_to_field, field)) {
                return false;
            }
            if (bw.lower->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
                bw.upper->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                return false; // function/cast/non-literal bound -> residual
            }
            const Value &lo_val = bw.lower->Cast<BoundConstantExpression>().value;
            const Value &hi_val = bw.upper->Cast<BoundConstantExpression>().value;
            if (lo_val.IsNull() || hi_val.IsNull()) {
                // Degenerate range (`field >= null`): SoqlLiteral would emit the
                // SOQL `null` keyword, which is only valid for = / != null. Leave
                // residual; DuckDB handles the NULL-bound semantics correctly.
                return false;
            }
            string lo_op, hi_op;
            if (!ComparisonOp(bw.LowerComparisonType(), lo_op) ||
                !ComparisonOp(bw.UpperComparisonType(), hi_op)) {
                return false;
            }
            string lo = SoqlLiteral(lo_val);
            string hi = SoqlLiteral(hi_val);
            out = "(" + field + " " + lo_op + " " + lo + " AND " + field + " " + hi_op +
                  " " + hi + ")";
            return true;
        }

        // IS NULL / IS NOT NULL (exact).
        if (klass == ExpressionClass::BOUND_OPERATOR &&
            (etype == ExpressionType::OPERATOR_IS_NULL ||
             etype == ExpressionType::OPERATOR_IS_NOT_NULL)) {
            auto &oe = expr.Cast<BoundOperatorExpression>();
            if (oe.children.size() != 1) {
                return false;
            }
            string field;
            if (!FieldFor(*oe.children[0], fields, projection_to_field, field)) {
                return false;
            }
            out = field +
                  (etype == ExpressionType::OPERATOR_IS_NULL ? " = null" : " != null");
            return true;
        }

        // col IN (c1, c2, ...) — SUPERSET prefilter, keep residual.
        if (klass == ExpressionClass::BOUND_OPERATOR && etype == ExpressionType::COMPARE_IN) {
            auto &oe = expr.Cast<BoundOperatorExpression>();
            if (oe.children.size() < 2) {
                return false;
            }
            string field;
            if (!FieldFor(*oe.children[0], fields, projection_to_field, field)) {
                return false;
            }
            if (oe.children.size() - 1 > kMaxInItems) {
                return false; // huge IN -> residual
            }
            string list;
            for (idx_t k = 1; k < oe.children.size(); k++) {
                if (oe.children[k]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                    return false;
                }
                if (k > 1) {
                    list += ", ";
                }
                list += SoqlLiteral(oe.children[k]->Cast<BoundConstantExpression>().value);
            }
            out = field + " IN (" + list + ")";
            exact = false;
            return true;
        }

        // LIKE. DuckDB's optimizer rewrites col LIKE 'A%' -> prefix(col,'A'),
        // '%z' -> suffix(col,'z'), '%m%' -> contains(col,'m'); complex patterns
        // stay as the "~~" function. Map each back to a SOQL LIKE. Treating any
        // literal % / _ in the substring as wildcards only BROADENS the match
        // (superset) -> safe because we keep it residual. SF LIKE is also
        // case-insensitive (another superset). Keep residual.
        if (klass == ExpressionClass::BOUND_FUNCTION) {
            auto &fe = expr.Cast<BoundFunctionExpression>();
            const string &fn = fe.function.name;
            bool like = (fn == "~~"), pre = (fn == "prefix"), suf = (fn == "suffix"),
                 con = (fn == "contains");
            if (!(like || pre || suf || con) || fe.children.size() != 2) {
                return false;
            }
            string field;
            if (!FieldFor(*fe.children[0], fields, projection_to_field, field)) {
                return false;
            }
            if (fe.children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
                return false;
            }
            const Value &v = fe.children[1]->Cast<BoundConstantExpression>().value;
            if (v.IsNull() || v.type().id() != LogicalTypeId::VARCHAR) {
                return false;
            }
            string sub = StringValue::Get(v);
            string pattern = like ? sub : pre ? sub + "%" : suf ? "%" + sub : "%" + sub + "%";
            out = field + " LIKE " + SoqlString(pattern);
            exact = false;
            return true;
        }

        // (a OR b ...) — push only if EVERY child translates; SUPERSET, keep residual.
        // (a AND b ...) — nested AND (e.g. inside an OR); exact iff all children exact.
        if (klass == ExpressionClass::BOUND_CONJUNCTION &&
            (etype == ExpressionType::CONJUNCTION_OR ||
             etype == ExpressionType::CONJUNCTION_AND)) {
            auto &cj = expr.Cast<BoundConjunctionExpression>();
            bool all_exact = true;
            string combined;
            for (idx_t k = 0; k < cj.children.size(); k++) {
                string cout;
                bool cexact = true;
                if (!TranslateExpr(*cj.children[k], fields, projection_to_field, cout, cexact)) {
                    return false; // any untranslatable child -> whole conjunction residual
                }
                all_exact = all_exact && cexact;
                if (k > 0) {
                    combined += (etype == ExpressionType::CONJUNCTION_OR) ? " OR " : " AND ";
                }
                combined += cout;
            }
            out = "(" + combined + ")";
            // OR is always a superset prefilter; AND is exact only if all children are.
            exact = (etype == ExpressionType::CONJUNCTION_AND) && all_exact;
            return true;
        }

        return false; // functions/casts/NOT/regex/... -> residual
    } catch (...) {
        return false;
    }
}

void PushdownToSoql(const vector<SalesforceField> &fields,
                    const vector<idx_t> &projection_to_field, string &out_where,
                    vector<unique_ptr<Expression>> &filters,
                    vector<PushdownConjunctInfo> *out_info) {
    out_where.clear();
    // Per conjunct: was it translated, and is it EXACT (safe to remove from the
    // residual set) or a superset prefilter (must stay residual)?
    vector<bool> translated(filters.size(), false);
    vector<bool> exact(filters.size(), false);
    vector<string> clauses;
    for (idx_t i = 0; i < filters.size(); i++) {
        string clause;
        bool ex = true;
        if (filters[i] && TranslateExpr(*filters[i], fields, projection_to_field, clause, ex)) {
            clauses.push_back(clause);
            translated[i] = true;
            exact[i] = ex;
        }
    }
    string where;
    for (idx_t i = 0; i < clauses.size(); i++) {
        if (i > 0) {
            where += " AND ";
        }
        where += clauses[i];
    }
    if (where.size() > kMaxWhereChars) {
        // guard: over-long -> push nothing, leave ALL residual. The diagnostic
        // must reflect reality: nothing was pushed.
        if (out_info) {
            out_info->assign(filters.size(), PushdownConjunctInfo{});
        }
        return;
    }
    if (out_info) {
        out_info->resize(filters.size());
        for (idx_t i = 0; i < filters.size(); i++) {
            (*out_info)[i] = PushdownConjunctInfo{translated[i], exact[i]};
        }
    }
    out_where = where;
    // Remove ONLY exact-translated conjuncts; superset prefilters (IN/LIKE/OR)
    // stay in `filters` so DuckDB reapplies them and refines the result.
    vector<unique_ptr<Expression>> remaining;
    for (idx_t i = 0; i < filters.size(); i++) {
        if (!(translated[i] && exact[i])) {
            remaining.push_back(std::move(filters[i]));
        }
    }
    filters = std::move(remaining);
}

string BuildSelectSoql(const string &object, const vector<string> &select_fields,
                       const string &where_clause, optional_idx limit) {
    string soql = "SELECT ";
    for (idx_t i = 0; i < select_fields.size(); i++) {
        if (i > 0) {
            soql += ", ";
        }
        soql += select_fields[i];
    }
    soql += " FROM " + object;
    if (!where_clause.empty()) {
        soql += " WHERE " + where_clause;
    }
    if (limit.IsValid()) {
        soql += " LIMIT " + std::to_string(limit.GetIndex());
    }
    return soql;
}

// --- last-SOQL diagnostic ----------------------------------------------------

static std::mutex g_soql_lock;
static string g_last_soql;

void SetLastSoql(const string &soql) {
    std::lock_guard<std::mutex> g(g_soql_lock);
    g_last_soql = soql;
}

namespace {

struct LastSoqlGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> LastSoqlBind(ClientContext &, TableFunctionBindInput &,
                                             vector<LogicalType> &return_types,
                                             vector<string> &names) {
    names = {"soql"};
    return_types = {LogicalType::VARCHAR};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> LastSoqlInit(ClientContext &,
                                                         TableFunctionInitInput &) {
    return make_uniq<LastSoqlGlobalState>();
}

static void LastSoqlFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<LastSoqlGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    string soql;
    {
        std::lock_guard<std::mutex> g(g_soql_lock);
        soql = g_last_soql;
    }
    FlatVector::GetData<string_t>(output.data[0])[0] =
        StringVector::AddString(output.data[0], soql);
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceLastSoqlFunction() {
    return TableFunction("salesforce_last_soql", {}, LastSoqlFunction, LastSoqlBind, LastSoqlInit);
}

// --- last-scan-pages diagnostic (DEBUG/TEST ONLY) ----------------------------

static std::mutex g_pages_lock;
static int64_t g_last_scan_pages = 0;

void SetLastScanPages(idx_t pages) {
    std::lock_guard<std::mutex> g(g_pages_lock);
    g_last_scan_pages = static_cast<int64_t>(pages);
}

namespace {

struct LastPagesGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> LastPagesBind(ClientContext &, TableFunctionBindInput &,
                                              vector<LogicalType> &return_types,
                                              vector<string> &names) {
    names = {"pages"};
    return_types = {LogicalType::BIGINT};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> LastPagesInit(ClientContext &,
                                                          TableFunctionInitInput &) {
    return make_uniq<LastPagesGlobalState>();
}

static void LastPagesFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<LastPagesGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    int64_t pages;
    {
        std::lock_guard<std::mutex> g(g_pages_lock);
        pages = g_last_scan_pages;
    }
    FlatVector::GetData<int64_t>(output.data[0])[0] = pages;
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceLastScanPagesFunction() {
    return TableFunction("salesforce_last_scan_pages", {}, LastPagesFunction, LastPagesBind,
                         LastPagesInit);
}

// --- last Bulk create-job body diagnostic (DEBUG/TEST ONLY) ------------------

static std::mutex g_bulk_body_lock;
static string g_last_bulk_create_body;

// Accumulates one entry per Bulk job-create in a scan (PK chunking, #v0.7 §9
// can create several). Newline-separated; reset at the start of each Bulk scan.
void SetLastBulkCreateBody(const string &body) {
    std::lock_guard<std::mutex> g(g_bulk_body_lock);
    if (!g_last_bulk_create_body.empty()) {
        g_last_bulk_create_body += "\n";
    }
    g_last_bulk_create_body += body;
}

void ResetBulkCreateBodies() {
    std::lock_guard<std::mutex> g(g_bulk_body_lock);
    g_last_bulk_create_body.clear();
}

namespace {

struct LastBulkBodyGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> LastBulkBodyBind(ClientContext &, TableFunctionBindInput &,
                                                 vector<LogicalType> &return_types,
                                                 vector<string> &names) {
    names = {"body"};
    return_types = {LogicalType::VARCHAR};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> LastBulkBodyInit(ClientContext &,
                                                             TableFunctionInitInput &) {
    return make_uniq<LastBulkBodyGlobalState>();
}

static void LastBulkBodyFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<LastBulkBodyGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    string body;
    {
        std::lock_guard<std::mutex> g(g_bulk_body_lock);
        body = g_last_bulk_create_body;
    }
    FlatVector::GetData<string_t>(output.data[0])[0] =
        StringVector::AddString(output.data[0], body);
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceLastBulkCreateBodyFunction() {
    return TableFunction("salesforce_last_bulk_create_body", {}, LastBulkBodyFunction,
                         LastBulkBodyBind, LastBulkBodyInit);
}

// --- last transport-selection diagnostic (DEBUG/TEST ONLY) -------------------

static std::mutex g_transport_lock;
static string g_last_transport;
static int64_t g_last_est_rows = -1; // -1 -> no probe ran (emitted as NULL)
static string g_last_transport_reason;

void SetLastTransport(const string &transport, int64_t est_rows, const string &reason) {
    std::lock_guard<std::mutex> g(g_transport_lock);
    g_last_transport = transport;
    g_last_est_rows = est_rows;
    g_last_transport_reason = reason;
}

namespace {

struct LastTransportGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> LastTransportBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types,
                                                  vector<string> &names) {
    names = {"transport", "est_rows", "reason"};
    return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> LastTransportInit(ClientContext &,
                                                              TableFunctionInitInput &) {
    return make_uniq<LastTransportGlobalState>();
}

static void LastTransportFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<LastTransportGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    string transport, reason;
    int64_t est;
    {
        std::lock_guard<std::mutex> g(g_transport_lock);
        transport = g_last_transport;
        est = g_last_est_rows;
        reason = g_last_transport_reason;
    }
    FlatVector::GetData<string_t>(output.data[0])[0] =
        StringVector::AddString(output.data[0], transport);
    if (est < 0) {
        FlatVector::SetNull(output.data[1], 0, true);
    } else {
        FlatVector::GetData<int64_t>(output.data[1])[0] = est;
    }
    FlatVector::GetData<string_t>(output.data[2])[0] =
        StringVector::AddString(output.data[2], reason);
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceLastTransportFunction() {
    return TableFunction("salesforce_last_transport", {}, LastTransportFunction,
                         LastTransportBind, LastTransportInit);
}

// --- describe-call counter (DEBUG/TEST ONLY) ---------------------------------

static std::mutex g_describe_lock;
static int64_t g_describe_calls = 0;

void ResetDescribeCalls() {
    std::lock_guard<std::mutex> g(g_describe_lock);
    g_describe_calls = 0;
}

void IncDescribeCalls() {
    std::lock_guard<std::mutex> g(g_describe_lock);
    g_describe_calls++;
}

namespace {

struct DescribeCallsGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> DescribeCallsBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types,
                                                  vector<string> &names) {
    names = {"calls"};
    return_types = {LogicalType::BIGINT};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DescribeCallsInit(ClientContext &,
                                                              TableFunctionInitInput &) {
    return make_uniq<DescribeCallsGlobalState>();
}

static void DescribeCallsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<DescribeCallsGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    int64_t calls;
    {
        std::lock_guard<std::mutex> g(g_describe_lock);
        calls = g_describe_calls;
    }
    FlatVector::GetData<int64_t>(output.data[0])[0] = calls;
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceDescribeCallsFunction() {
    return TableFunction("salesforce_describe_calls", {}, DescribeCallsFunction,
                         DescribeCallsBind, DescribeCallsInit);
}

// --- tooling-query counter (DEBUG/TEST ONLY, #v0.6 §6) -----------------------

static std::mutex g_tooling_lock;
static int64_t g_tooling_calls = 0;

void ResetToolingCalls() {
    std::lock_guard<std::mutex> g(g_tooling_lock);
    g_tooling_calls = 0;
}

void IncToolingCalls() {
    std::lock_guard<std::mutex> g(g_tooling_lock);
    g_tooling_calls++;
}

namespace {

struct ToolingCallsGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> ToolingCallsBind(ClientContext &, TableFunctionBindInput &,
                                                 vector<LogicalType> &return_types,
                                                 vector<string> &names) {
    names = {"calls"};
    return_types = {LogicalType::BIGINT};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> ToolingCallsInit(ClientContext &,
                                                             TableFunctionInitInput &) {
    return make_uniq<ToolingCallsGlobalState>();
}

static void ToolingCallsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<ToolingCallsGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    int64_t calls;
    {
        std::lock_guard<std::mutex> g(g_tooling_lock);
        calls = g_tooling_calls;
    }
    FlatVector::GetData<int64_t>(output.data[0])[0] = calls;
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceToolingCallsFunction() {
    return TableFunction("salesforce_tooling_calls", {}, ToolingCallsFunction, ToolingCallsBind,
                         ToolingCallsInit);
}

// --- global-describe-call counter (DEBUG/TEST ONLY) --------------------------

static std::mutex g_global_lock;
static int64_t g_global_describe_calls = 0;

void ResetGlobalDescribeCalls() {
    std::lock_guard<std::mutex> g(g_global_lock);
    g_global_describe_calls = 0;
}

void IncGlobalDescribeCalls() {
    std::lock_guard<std::mutex> g(g_global_lock);
    g_global_describe_calls++;
}

namespace {

struct GlobalCallsGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> GlobalCallsBind(ClientContext &, TableFunctionBindInput &,
                                                vector<LogicalType> &return_types,
                                                vector<string> &names) {
    names = {"calls"};
    return_types = {LogicalType::BIGINT};
    return nullptr;
}

static unique_ptr<GlobalTableFunctionState> GlobalCallsInit(ClientContext &,
                                                            TableFunctionInitInput &) {
    return make_uniq<GlobalCallsGlobalState>();
}

static void GlobalCallsFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<GlobalCallsGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    int64_t calls;
    {
        std::lock_guard<std::mutex> g(g_global_lock);
        calls = g_global_describe_calls;
    }
    FlatVector::GetData<int64_t>(output.data[0])[0] = calls;
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

TableFunction GetSalesforceGlobalDescribeCallsFunction() {
    return TableFunction("salesforce_global_describe_calls", {}, GlobalCallsFunction,
                         GlobalCallsBind, GlobalCallsInit);
}

} // namespace duckdb

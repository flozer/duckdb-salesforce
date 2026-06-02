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
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

#include <mutex>

namespace duckdb {

// Max generated WHERE length before we give up and residualise (Appendix A).
static constexpr size_t kMaxWhereChars = 4000;

string SoqlLiteral(const Value &value) {
    if (value.IsNull()) {
        return "null";
    }
    switch (value.type().id()) {
    case LogicalTypeId::VARCHAR: {
        // Single-quote + escape backslash and quote.
        const string &s = StringValue::Get(value);
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

// Translate one conjunct into a SOQL predicate. Returns false (=> residual) for
// anything outside the safe subset. Never throws.
static bool TranslateExpr(const Expression &expr, const vector<SalesforceField> &fields,
                          const vector<idx_t> &projection_to_field, string &out) {
    try {
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON) {
            auto &cmp_expr = expr.Cast<BoundComparisonExpression>();
            const Expression &l = *cmp_expr.left;
            const Expression &r = *cmp_expr.right;

            string field;
            ExpressionType cmp = expr.GetExpressionType();
            const Expression *constant = nullptr;
            if (FieldFor(l, fields, projection_to_field, field) &&
                r.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                constant = &r;
            } else if (FieldFor(r, fields, projection_to_field, field) &&
                       l.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
                if (!FlipComparison(expr.GetExpressionType(), cmp)) {
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
        if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
            (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ||
             expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL)) {
            auto &oe = expr.Cast<BoundOperatorExpression>();
            if (oe.children.size() != 1) {
                return false;
            }
            string field;
            if (!FieldFor(*oe.children[0], fields, projection_to_field, field)) {
                return false;
            }
            out = field +
                  (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ? " = null" : " != null");
            return true;
        }
        return false; // OR / IN / LIKE / functions / ... -> residual
    } catch (...) {
        return false;
    }
}

void PushdownToSoql(const vector<SalesforceField> &fields,
                    const vector<idx_t> &projection_to_field, string &out_where,
                    vector<unique_ptr<Expression>> &filters) {
    out_where.clear();
    vector<bool> handled(filters.size(), false);
    vector<string> clauses;
    for (idx_t i = 0; i < filters.size(); i++) {
        string clause;
        if (filters[i] && TranslateExpr(*filters[i], fields, projection_to_field, clause)) {
            clauses.push_back(clause);
            handled[i] = true;
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
        return; // guard: over-long -> push nothing, leave all residual
    }
    out_where = where;
    vector<unique_ptr<Expression>> remaining;
    for (idx_t i = 0; i < filters.size(); i++) {
        if (!handled[i]) {
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

} // namespace duckdb

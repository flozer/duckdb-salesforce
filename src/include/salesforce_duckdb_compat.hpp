// Dual-line compatibility layer (DuckDB 1.5.x + 2.0).
//
// SF_DUCKDB_V2 is defined by our CMakeLists when DUCKDB_VERSION is 2.x. Every
// helper here compiles on BOTH lines; version-divergent expression-tree code
// (soql/storage/agg optimizer) stays gated at its call sites with #ifdef
// SF_DUCKDB_V2.
#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/common/types/vector.hpp"

#ifdef SF_DUCKDB_V2
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#endif

#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace sf_compat {

// The names vector of a table-function bind callback: vector<string> on
// 1.5.x, vector<Identifier> on 2.0. Identifier -> string is implicit on
// 1.5.x, so Identifier values push into either flavor.
#ifdef SF_DUCKDB_V2
using names_t = duckdb::vector<duckdb::Identifier>;
#else
using names_t = duckdb::vector<std::string>;
#endif

inline std::string name_of(const duckdb::Identifier &id) {
	return id.GetIdentifierName();
}
inline std::string name_of(const std::string &s) {
	return s;
}

// Named-parameter registration (kwargs on 2.0, the map member on 1.5.x).
inline void set_named_params(duckdb::TableFunction &fn,
                             std::initializer_list<std::pair<const char *, duckdb::LogicalType>> params) {
#ifdef SF_DUCKDB_V2
	fn.GetSignature().WithTypedKwargs("options", [&](duckdb::TypedKwargs &kw) {
		for (auto &p : params) {
			kw.Add(duckdb::Identifier(p.first), p.second);
		}
	});
#else
	for (auto &p : params) {
		fn.named_parameters[p.first] = p.second;
	}
#endif
}

// Optional trailing varargs positional parameter.
inline void set_varargs(duckdb::TableFunction &fn, const char *name, duckdb::LogicalType type) {
#ifdef SF_DUCKDB_V2
	fn.GetSignature().AddArgs(duckdb::Identifier(name), type);
#else
	(void)name;
	fn.varargs = type;
#endif
}

// Positional parameter NAME on the signature (2.0 surfaces these through
// duckdb_functions(); 1.5.x takes them from the FunctionDescription).
template <typename Fn>
inline void set_param_name(Fn &fn, duckdb::idx_t i, const char *name) {
#ifdef SF_DUCKDB_V2
	fn.GetSignature().GetParameter(i).SetName(duckdb::Identifier(name));
#else
	(void)fn;
	(void)i;
	(void)name;
#endif
}

// Writable flat data: 2.0's FlatVector::GetData is const-qualified.
template <typename T>
inline T *flat_data_mutable(duckdb::Vector &vector) {
#ifdef SF_DUCKDB_V2
	return duckdb::FlatVector::GetDataMutable<T>(vector);
#else
	return duckdb::FlatVector::GetData<T>(vector);
#endif
}

// Struct entries: vector<unique_ptr<Vector>> on 1.5.x, vector<Vector> on 2.0 —
// normalize to a raw pointer per entry.
inline duckdb::Vector *struct_entry(duckdb::Vector &vec, duckdb::idx_t i) {
#ifdef SF_DUCKDB_V2
	return &duckdb::StructVector::GetEntries(vec)[i];
#else
	return duckdb::StructVector::GetEntries(vec)[i].get();
#endif
}

inline duckdb::ColumnBinding colref_binding(const duckdb::BoundColumnRefExpression &ref) {
#ifdef SF_DUCKDB_V2
	return ref.Binding();
#else
	return ref.binding;
#endif
}

// BoundConstantExpression::value (1.5.x) / GetValue() (2.0).
inline const duckdb::Value &const_value(const duckdb::Expression &expr) {
#ifdef SF_DUCKDB_V2
	return expr.Cast<duckdb::BoundConstantExpression>().GetValue();
#else
	return expr.Cast<duckdb::BoundConstantExpression>().value;
#endif
}

// children member (1.5.x) / GetChildren() (2.0).
inline const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &
children_of(const duckdb::BoundOperatorExpression &expr) {
#ifdef SF_DUCKDB_V2
	return expr.GetChildren();
#else
	return expr.children;
#endif
}
inline const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &
children_of(const duckdb::BoundConjunctionExpression &expr) {
#ifdef SF_DUCKDB_V2
	return expr.GetChildren();
#else
	return expr.children;
#endif
}
inline const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &
children_of(const duckdb::BoundFunctionExpression &expr) {
#ifdef SF_DUCKDB_V2
	return expr.GetChildren();
#else
	return expr.children;
#endif
}

// Bound function name as std::string.
inline std::string function_name(const duckdb::BoundFunctionExpression &expr) {
#ifdef SF_DUCKDB_V2
	return expr.Function().GetDefinition()->GetName().GetIdentifierName();
#else
	return expr.function.name;
#endif
}

} // namespace sf_compat

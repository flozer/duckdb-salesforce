// User-callable scan (consumer feedback #3; roadmap backlog B).
//
// salesforce_scan(catalog, object [, where]) — a direct, one-off scan over an
// sObject using the credentials of an attached catalog, without going through
// the catalog table layer. Optional named parameters:
//   where:      raw SOQL predicate appended to the scan (AND-ed with any
//               plan-pushed predicates); validated like salesforce_aggregate
//               (no ';', no nested SELECT) — same exposure level.
//   query_mode: 'query' | 'queryAll' per-call override (no session leak).
//   transport:  'rest' | 'bulk' | 'auto' per-call override.
//   chunks:     Bulk PK-chunking count per-call override.
//
// The schema comes from the sObject describe at bind time (bind may do
// network, same precedent as salesforce_describe): flat fields only (no
// parent-relationship STRUCT expansion — use catalog tables for that).
// Projection/predicate pushdown and the plan-pushdown safety rules apply
// exactly as on catalog tables.

#include "salesforce_scan.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <cctype>

namespace duckdb {

// Same safety contract as salesforce_aggregate's RejectUnsafe: the raw where
// is user-provided SOQL — reject statement separators and nested SELECTs.
static void RejectUnsafeWhere(const string &where) {
	if (where.size() > 4000) {
		throw BinderException("salesforce_scan: where is too long (max 4000 chars) - narrow it or "
		                      "use a catalog table with plan pushdown.");
	}
	if (where.find(';') != string::npos) {
		throw BinderException("salesforce_scan: where must not contain ';'.");
	}
	if (StringUtil::Contains(StringUtil::Upper(where), "SELECT")) {
		throw BinderException("salesforce_scan: where must not contain a nested SELECT.");
	}
}

unique_ptr<SalesforceScanBindData> BuildUserScanBindData(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs.size() != 2) {
		throw BinderException("salesforce_scan(catalog, object) takes exactly 2 arguments "
		                      "(where/query_mode/transport/chunks are named parameters).");
	}
	for (auto &a : input.inputs) {
		if (a.IsNull()) {
			throw BinderException("salesforce_scan: arguments must not be NULL.");
		}
	}
	string alias = StringUtil::Lower(input.inputs[0].ToString());
	string object = StringUtil::Upper(input.inputs[1].ToString());
	bool safe_object = !object.empty();
	for (auto &ch : object) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
			safe_object = false;
			break;
		}
	}
	if (!safe_object) {
		throw BinderException("salesforce_scan: '%s' is not a valid sObject name.", object);
	}
	string where;
	auto wh = input.named_parameters.find("filter");
	if (wh != input.named_parameters.end() && !wh->second.IsNull()) {
		where = wh->second.ToString();
		RejectUnsafeWhere(where);
	}

	SalesforceConfig config;
	SalesforceTokenSet token;
	GetSalesforceCatalogCredentials(context, alias, config, token);

	// Per-call overrides of the session settings (validated, stored on the
	// bind config copy — scan init prefers them over session settings).
	auto qm = input.named_parameters.find("query_mode");
	if (qm != input.named_parameters.end() && !qm->second.IsNull()) {
		string v = StringUtil::Lower(qm->second.ToString());
		if (v != "query" && v != "queryall") {
			throw BinderException("salesforce_scan: query_mode must be 'query' or 'queryAll' (got '%s').",
			                      qm->second.ToString());
		}
		config.query_mode = v == "queryall" ? "queryAll" : v;
	}
	auto tp = input.named_parameters.find("transport");
	if (tp != input.named_parameters.end() && !tp->second.IsNull()) {
		string v = StringUtil::Lower(tp->second.ToString());
		if (v != "rest" && v != "bulk" && v != "auto") {
			throw BinderException("salesforce_scan: transport must be 'rest', 'bulk' or 'auto' (got '%s').",
			                      tp->second.ToString());
		}
		config.transport = v;
	}
	auto bc = input.named_parameters.find("chunks");
	if (bc != input.named_parameters.end() && !bc->second.IsNull()) {
		int64_t chunks = bc->second.GetValue<int64_t>();
		if (chunks < 1 || chunks > 8) {
			throw BinderException("salesforce_scan: chunks must be between 1 and 8.");
		}
		config.bulk_chunks = chunks;
	}

	auto client = BuildHttpClientForContext(context);
	SalesforceSession session(config, *client);
	session.SetToken(token);
	SalesforceDescribe describe = session.Describe(object); // network at bind

	auto data = make_uniq<SalesforceScanBindData>();
	data->config = config;
	data->token = token;
	data->object = object;
	data->fields = describe.fields;
	for (auto &f : data->fields) {
		data->column_names.push_back(f.name);
		data->column_types.push_back(f.duckdb_type);
	}
	data->raw_where = where;
	return std::move(data);
}

} // namespace duckdb

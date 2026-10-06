// Bulk job resume (consumer feedback #4; roadmap backlog).
//
// salesforce_bulk_resume(catalog, job_id) re-opens the result pages of an
// existing Bulk API 2.0 query job (typically in state JobComplete) and emits
// its rows — the recovery path when a load dies mid-stream of a job that
// already finished server-side, so the retry does not re-create the job and
// re-run the query. The FIRST result page is fetched at bind time (bind may do
// network; salesforce_describe does the same) and its CSV header defines the
// output schema (one VARCHAR column per header field — Bulk CSV carries no
// type metadata). Rows stream lazily page-by-page following the
// Sforce-Locator. Read-only; credentials from the attached catalog.
//
// Errors from Salesforce surface with the errorCode + remedy hints (the
// session's error path), e.g. an invalid/expired job id fails fast with a
// clear message.

#include "salesforce_bulk_resume.hpp"
#include "salesforce_http.hpp"
#include "salesforce_session.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

#include <cctype>

namespace duckdb {

namespace {

struct BulkResumeBindData : public TableFunctionData {
	string alias;
	string job_id;
	string api_version;
	string results_base;      // /jobs/query/<id>/results (locator appended)
	SalesforceBulkPage first; // first page fetched at bind (locator follows)
	vector<string> column_names;
	vector<LogicalType> column_types;

	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<BulkResumeBindData>();
		r->alias = alias;
		r->job_id = job_id;
		r->api_version = api_version;
		r->results_base = results_base;
		r->first = first;
		r->column_names = column_names;
		r->column_types = column_types;
		return std::move(r);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<BulkResumeBindData>();
		return alias == other.alias && job_id == other.job_id;
	}
};

struct BulkResumeGlobalState : public GlobalTableFunctionState {
	unique_ptr<SalesforceHttpClient> client; // declared before session
	unique_ptr<SalesforceSession> session;
	string results_base;     // /jobs/query/<id>/results (locator appended)
	SalesforceBulkPage page; // current page being emitted
	bool have_page = false;  // bind owns the first page; copied on first call
	size_t cursor = 0;
	idx_t max_threads = 1;
	idx_t MaxThreads() const override {
		return max_threads;
	}
};

unique_ptr<FunctionData> BulkResumeBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2) {
		throw BinderException("salesforce_bulk_resume(catalog, job_id) takes exactly 2 arguments.");
	}
	for (auto &a : input.inputs) {
		if (a.IsNull()) {
			throw BinderException("salesforce_bulk_resume: arguments must not be NULL.");
		}
	}
	auto data = make_uniq<BulkResumeBindData>();
	data->alias = StringUtil::Lower(input.inputs[0].ToString());
	data->job_id = input.inputs[1].ToString();
	bool safe_job = data->job_id.size() >= 15 && data->job_id.size() <= 32;
	for (auto &ch : data->job_id) {
		if (!std::isalnum(static_cast<unsigned char>(ch))) {
			safe_job = false;
			break;
		}
	}
	if (!safe_job) {
		throw BinderException("salesforce_bulk_resume: '%s' is not a valid Bulk job id "
		                      "(expected a 15-32 char alphanumeric id).",
		                      data->job_id);
	}

	// Credentials from the attached catalog; first result page fetched HERE so
	// the CSV header defines the output schema (bind may do network, same
	// precedent as salesforce_describe).
	SalesforceConfig config;
	SalesforceTokenSet token;
	GetSalesforceCatalogCredentials(context, data->alias, config, token);
	auto client = BuildHttpClientForContext(context);
	SalesforceSession session(config, *client);
	session.SetToken(token);
	data->api_version = config.api_version;
	data->results_base = "/services/data/" + data->api_version + "/jobs/query/" + data->job_id + "/results";
	data->first = session.BulkFetchResultPage(data->results_base); // throws (w/ hints) on bad job state

	for (auto &col : data->first.columns) {
		data->column_names.push_back(col);
		data->column_types.push_back(LogicalType::VARCHAR);
	}
	return_types = data->column_types;
	names = data->column_names;
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> BulkResumeInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<BulkResumeBindData>();
	auto gstate = make_uniq<BulkResumeGlobalState>();
	auto config = make_uniq<SalesforceConfig>();
	SalesforceTokenSet token;
	GetSalesforceCatalogCredentials(context, bind.alias, *config, token);
	gstate->client = BuildHttpClientForContext(context);
	gstate->session = make_uniq<SalesforceSession>(*config, *gstate->client);
	gstate->session->SetToken(token);
	gstate->results_base = bind.results_base;
	// First page travels with the bind data; emit it before fetching more.
	gstate->page = bind.first;
	gstate->have_page = true;
	return std::move(gstate);
}

void BulkResumeFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<BulkResumeBindData>();
	auto &gstate = data.global_state->Cast<BulkResumeGlobalState>();

	while (true) {
		if (gstate.cursor < gstate.page.rows.size()) {
			idx_t row = 0;
			while (row < STANDARD_VECTOR_SIZE && gstate.cursor < gstate.page.rows.size()) {
				auto &cells = gstate.page.rows[gstate.cursor];
				for (idx_t c = 0; c < output.ColumnCount(); c++) {
					if (c < cells.size() && !cells[c].empty()) {
						FlatVector::GetData<string_t>(output.data[c])[row] =
						    StringVector::AddString(output.data[c], cells[c]);
					} else {
						FlatVector::SetNull(output.data[c], row, true);
					}
				}
				gstate.cursor++;
				row++;
			}
			output.SetCardinality(row);
			return;
		}
		// Page exhausted: follow the locator, or stop.
		if (gstate.page.next_locator.empty()) {
			output.SetCardinality(0);
			return;
		}
		gstate.page = gstate.session->BulkFetchResultPage(gstate.results_base + "/" + gstate.page.next_locator);
		gstate.cursor = 0;
	}
}

} // namespace

TableFunction GetSalesforceBulkResumeFunction() {
	TableFunction fn("salesforce_bulk_resume", {LogicalType::VARCHAR, LogicalType::VARCHAR}, BulkResumeFunction,
	                 BulkResumeBind, BulkResumeInit);
	return fn;
}

} // namespace duckdb

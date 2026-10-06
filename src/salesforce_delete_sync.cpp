// Delete-sync primitive (consumer feedback #1; roadmap cycle 2).
//
// salesforce_deleted_ids(catalog, object [, since] [, until]) surfaces the
// ids deleted in the org within a time window, so a watermark-based
// incremental load can pair its upsert with a delete sweep. WITHOUT this, a
// watermark load never notices deletes: deletion does not update
// SystemModstamp, and live-row queries exclude the deleted rows.
//
// Two sources, chosen by window width:
//  - window <= 15 minutes: Replication API getDeleted()
//    (GET /sobjects/<object>/deleted/?start=<ISO>&end=<ISO>) — exact
//    deletedDate per id, source='getDeleted';
//  - wider window: queryAll scan `WHERE IsDeleted = true AND SystemModstamp
//    >= since` — one call, but the deletion timestamp is NOT recoverable
//    (emitted as NULL, source='queryAll') and the recycle bin holds rows
//    only ~15 days, so run the sweep periodically.
//
// Read-only. Credentials come from the attached catalog (auth_source any).

#include "salesforce_delete_sync.hpp"
#include "salesforce_config.hpp"
#include "salesforce_json.hpp"
#include "salesforce_metadata_engine.hpp"
#include "salesforce_session.hpp"
#include "salesforce_http.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <chrono>
#include <cctype>
#include <ctime>
#include <mutex>

namespace duckdb {

namespace {

struct DeletedIdsBindData : public TableFunctionData {
	string alias;
	string object;
	string since; // ISO-8601 (already normalized/validated at bind)
	string until;
	bool use_get_deleted = false; // window <= 15 minutes

	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<DeletedIdsBindData>();
		r->alias = alias;
		r->object = object;
		r->since = since;
		r->until = until;
		r->use_get_deleted = use_get_deleted;
		return std::move(r);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DeletedIdsBindData>();
		return alias == other.alias && object == other.object && since == other.since && until == other.until;
	}
};

struct DeletedIdsGlobalState : public GlobalTableFunctionState {
	unique_ptr<SalesforceHttpClient> client; // declared before session
	unique_ptr<SalesforceSession> session;
	string api_version;

	vector<pair<string, string>> rows; // (id, deleted_date; date empty in queryAll fallback)
	size_t cursor = 0;
	bool loaded = false;
	bool failed = false;
	string error;
	idx_t max_threads = 1;
	idx_t MaxThreads() const override {
		return max_threads;
	}
};

// Current UTC time as Salesforce ISO-8601 ("2026-09-30T12:34:56Z").
string NowIsoUtc() {
	auto now = std::chrono::system_clock::now();
	std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm tm_utc {};
#ifdef _WIN32
	gmtime_s(&tm_utc, &tt);
#else
	gmtime_r(&tt, &tm_utc);
#endif
	char buf[32];
	size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	return string(buf, n);
}

// Parse "YYYY-MM-DD[THH:MM:SS[Z]]" (also accepts a space separator) into a
// time_point; throws BinderException with the caller's label on garbage.
std::chrono::system_clock::time_point ParseIsoUtc(const string &value, const string &label) {
	string v = value;
	if (!v.empty() && v.back() == 'Z') {
		v.pop_back();
	}
	auto space = v.find(' ');
	if (space != string::npos) {
		v[space] = 'T';
	}
	std::tm tm {};
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (sscanf(v.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) < 3) {
		throw BinderException("salesforce_deleted_ids: %s must be an ISO-8601 timestamp "
		                      "(YYYY-MM-DD[THH:MM:SS]); got '%s'.",
		                      label, value);
	}
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
#ifdef _WIN32
	std::time_t tt = _mkgmtime(&tm);
#else
	std::time_t tt = timegm(&tm);
#endif
	if (tt == static_cast<std::time_t>(-1)) {
		throw BinderException("salesforce_deleted_ids: %s is not a valid timestamp ('%s').", label, value);
	}
	return std::chrono::system_clock::from_time_t(tt);
}

string ToIsoUtc(std::chrono::system_clock::time_point tp) {
	std::time_t tt = std::chrono::system_clock::to_time_t(tp);
	std::tm tm_utc {};
#ifdef _WIN32
	gmtime_s(&tm_utc, &tt);
#else
	gmtime_r(&tt, &tm_utc);
#endif
	char buf[32];
	size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
	return string(buf, n);
}

// Trim an API timestamp ("2026-09-30T12:34:56.000+0000") to second precision
// UTC-ish output for the deleted_date column.
string NormalizeDeletedDate(const string &raw) {
	if (raw.size() < 19) {
		return raw;
	}
	// "YYYY-MM-DDTHH:MM:SS" prefix passes through; keep as-is (ISO-8601).
	return raw.substr(0, 19);
}

unique_ptr<FunctionData> DeletedIdsBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	auto &args = input.inputs;
	if (args.size() < 2 || args.size() > 4) {
		throw BinderException("salesforce_deleted_ids(catalog, object [, since] [, until]) takes 2 to 4 arguments.");
	}
	for (auto &a : args) {
		if (a.IsNull()) {
			throw BinderException("salesforce_deleted_ids: arguments must not be NULL.");
		}
	}
	string alias = StringUtil::Lower(args[0].ToString());
	string object = StringUtil::Upper(args[1].ToString());
	bool safe_object = !object.empty();
	for (auto &ch : object) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
			safe_object = false;
			break;
		}
	}
	if (!safe_object) {
		throw BinderException("salesforce_deleted_ids: '%s' is not a valid sObject name.", object);
	}

	auto now = std::chrono::system_clock::now();
	auto until_tp = now;
	auto since_tp = now - std::chrono::minutes(15);
	// since/until are NAMED parameters — they arrive via named_parameters,
	// never through the positional input list.
	auto since_it = input.named_parameters.find("since");
	if (since_it != input.named_parameters.end() && !since_it->second.IsNull()) {
		since_tp = ParseIsoUtc(since_it->second.ToString(), "since");
	}
	auto until_it = input.named_parameters.find("until");
	if (until_it != input.named_parameters.end() && !until_it->second.IsNull()) {
		until_tp = ParseIsoUtc(until_it->second.ToString(), "until");
	}
	if (since_tp >= until_tp) {
		throw BinderException("salesforce_deleted_ids: 'since' must be earlier than 'until'.");
	}

	auto data = make_uniq<DeletedIdsBindData>();
	data->alias = alias;
	data->object = object;
	data->since = ToIsoUtc(since_tp);
	data->until = ToIsoUtc(until_tp);
	// getDeleted() windows are capped at 15 minutes by Salesforce; wider
	// windows switch to the queryAll fallback (single call, no exact dates).
	data->use_get_deleted = (until_tp - since_tp) <= std::chrono::minutes(15);

	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"id", "deleted_date", "source"};
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> DeletedIdsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DeletedIdsBindData>();
	auto gstate = make_uniq<DeletedIdsGlobalState>();
	auto config = make_uniq<SalesforceConfig>();
	SalesforceTokenSet token;
	GetSalesforceCatalogCredentials(context, bind.alias, *config, token);
	gstate->client = BuildHttpClientForContext(context);
	gstate->session = make_uniq<SalesforceSession>(*config, *gstate->client);
	gstate->session->SetToken(token);
	gstate->api_version = config->api_version;
	return std::move(gstate);
}

void DeletedIdsFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<DeletedIdsBindData>();
	auto &gstate = data.global_state->Cast<DeletedIdsGlobalState>();

	if (!gstate.loaded) {
		gstate.loaded = true;
		if (bind.use_get_deleted) {
			// Slice [since, until] into <=15-minute getDeleted calls.
			auto since = ParseIsoUtc(bind.since, "since");
			auto until = ParseIsoUtc(bind.until, "until");
			const std::chrono::minutes kWindow(15);
			for (auto start = since; start < until; start += kWindow) {
				auto end = (until - start) < kWindow ? until : start + kWindow;
				string path = "/services/data/" + gstate.api_version + "/sobjects/" + bind.object +
				              "/deleted/?start=" + ToIsoUtc(start) + "&end=" + ToIsoUtc(end);
				string body;
				try {
					body = gstate.session->AuthorizedGet(path);
				} catch (std::exception &ex) {
					gstate.failed = true;
					gstate.error = ex.what();
					break;
				}
				for (auto &rec : sfjson::GetObjectArray(body, "deletedRecords")) {
					gstate.rows.emplace_back(sfjson::GetString(rec, "id"),
					                         NormalizeDeletedDate(sfjson::GetString(rec, "deletedDate")));
				}
			}
		} else {
			// queryAll fallback: one call; deletion timestamp not exposed by
			// Salesforce in query results (only the old SystemModstamp).
			string soql =
			    "SELECT Id FROM " + bind.object + " WHERE IsDeleted = true AND SystemModstamp >= " + bind.since;
			string path = "/services/data/" + gstate.api_version + "/queryAll/?q=" + StringUtil::URLEncode(soql);
			string body;
			try {
				body = gstate.session->AuthorizedGet(path);
			} catch (std::exception &ex) {
				gstate.failed = true;
				gstate.error = ex.what();
			}
			if (!gstate.failed) {
				for (auto &rec : sfjson::GetObjectArray(body, "records")) {
					gstate.rows.emplace_back(sfjson::GetString(rec, "Id"), "");
				}
			}
		}
		if (gstate.failed) {
			throw IOException("salesforce_deleted_ids failed: %s "
			                  "(fallback: SET sf_query_mode = 'queryAll'; and sweep "
			                  "WHERE IsDeleted = true yourself).",
			                  gstate.error);
		}
	}

	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE && gstate.cursor < gstate.rows.size()) {
		auto &id = gstate.rows[gstate.cursor].first;
		auto &deleted_date = gstate.rows[gstate.cursor].second;
		FlatVector::GetData<string_t>(output.data[0])[row] = StringVector::AddString(output.data[0], id);
		if (deleted_date.empty()) {
			FlatVector::SetNull(output.data[1], row, true);
		} else {
			FlatVector::GetData<string_t>(output.data[1])[row] = StringVector::AddString(output.data[1], deleted_date);
		}
		FlatVector::GetData<string_t>(output.data[2])[row] =
		    StringVector::AddString(output.data[2], bind.use_get_deleted ? "getDeleted" : "queryAll");
		gstate.cursor++;
		row++;
	}
	output.SetCardinality(row);
}

} // namespace

TableFunction GetSalesforceDeletedIdsFunction() {
	TableFunction fn("salesforce_deleted_ids", {LogicalType::VARCHAR, LogicalType::VARCHAR}, DeletedIdsFunction,
	                 DeletedIdsBind, DeletedIdsInit);
	fn.named_parameters["since"] = LogicalType::VARCHAR;
	fn.named_parameters["until"] = LogicalType::VARCHAR;
	return fn;
}

} // namespace duckdb

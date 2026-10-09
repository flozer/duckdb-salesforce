// Delete-sync primitive (consumer feedback #1; roadmap cycle 2 + fix cycle).
//
// salesforce_deleted_ids(catalog, object [, since] [, until] [, source])
// surfaces the ids deleted in the org within a time window, so a
// watermark-based incremental load can pair its upsert with a delete sweep.
// WITHOUT this, a watermark load never notices deletes: deletion does not
// update SystemModstamp, and live-row queries exclude the deleted rows.
//
// PRIMARY source (default): Replication API getDeleted()
//   GET /sobjects/<object>/deleted/?start=<ISO>&end=<ISO>
// Works for ANY window width (live-proven 2026-10-05: a 15-day window is
// accepted in one call; the 15-minute cap was an incorrect design assumption
// on our side), works for objects with no IsDeleted field (e.g. User), and
// carries exact deletedDate per id. Salesforce may cover less than the
// requested window on large result sets — the response's latestDateCovered
// says how far it got, and this function loops from there until the window is
// fully covered (bounded iterations).
//
// OPT-IN source ('queryAll'): single queryAll scan
//   WHERE IsDeleted = true AND SystemModstamp >= since
// — deletion timestamps are NOT exposed (emitted NULL); PAGINATED via
// nextRecordsUrl (the v0.19.0 release truncated at 2,000 rows — fixed here);
// recycle-bin retention ~15 days; objects with no IsDeleted field (User)
// fail with INVALID_FIELD.
//
// Read-only; credentials from the attached catalog.

#include "salesforce_delete_sync.hpp"
#include "salesforce_config.hpp"
#include "salesforce_http.hpp"
#include "salesforce_json.hpp"
#include "salesforce_session.hpp"
#include "salesforce_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <cctype>
#include <chrono>
#include <ctime>
#include <mutex>

namespace duckdb {

namespace {

constexpr int kGetDeletedMaxLoops = 10000;  // latestDateCovered loop guard
constexpr idx_t kQueryAllMaxPages = 100000; // pagination loop guard

struct DeletedIdsBindData : public TableFunctionData {
	string alias;
	string object;
	string since;                 // ISO-8601 (normalized/validated at bind)
	string until;                 // ISO-8601
	string source = "getDeleted"; // "getDeleted" (default) | "queryAll" (opt-in)

	unique_ptr<FunctionData> Copy() const override {
		auto r = make_uniq<DeletedIdsBindData>();
		r->alias = alias;
		r->object = object;
		r->since = since;
		r->until = until;
		r->source = source;
		return std::move(r);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<DeletedIdsBindData>();
		return alias == other.alias && object == other.object && since == other.since && until == other.until &&
		       source == other.source;
	}
};

struct DeletedIdsGlobalState : public GlobalTableFunctionState {
	unique_ptr<SalesforceHttpClient> client; // declared before session
	unique_ptr<SalesforceSession> session;
	string api_version;

	vector<pair<string, string>> rows; // (id, deleted_date; date empty in queryAll)
	string source;                     // per-row constant, mirrored from bind
	bool loaded = false;
	bool failed = false;
	string error;
	idx_t cursor = 0;
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
// for the deleted_date column.
string NormalizeDeletedDate(const string &raw) {
	if (raw.size() < 19) {
		return raw;
	}
	return raw.substr(0, 19);
}

unique_ptr<FunctionData> DeletedIdsBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.size() != 2) {
		throw BinderException("salesforce_deleted_ids(catalog, object) takes exactly 2 arguments "
		                      "(since/until/source are named parameters).");
	}
	for (auto &a : input.inputs) {
		if (a.IsNull()) {
			throw BinderException("salesforce_deleted_ids: arguments must not be NULL.");
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
		throw BinderException("salesforce_deleted_ids: '%s' is not a valid sObject name.", object);
	}

	auto now = std::chrono::system_clock::now();
	auto until_tp = now;
	auto since_tp = now - std::chrono::minutes(15);
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
	data->source = "getDeleted"; // queryAll available via the named source param
	auto src = input.named_parameters.find("source");
	if (src != input.named_parameters.end() && !src->second.IsNull()) {
		string v = StringUtil::Lower(src->second.ToString());
		if (v != "getdeleted" && v != "queryall") {
			throw BinderException("salesforce_deleted_ids: source must be 'getDeleted' or "
			                      "'queryAll' (got '%s').",
			                      src->second.ToString());
		}
		data->source = v == "queryall" ? "queryAll" : "getDeleted";
	}

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
	gstate->source = bind.source;
	return std::move(gstate);
}

// getDeleted: loop [since, until) in API-served slices via latestDateCovered.
// Salesforce may cover less than requested on large result sets; the response
// tells us how far it got, so we resume from there until the window is fully
// covered (bounded by kGetDeletedMaxLoops).
void LoadViaGetDeleted(DeletedIdsGlobalState &g, const DeletedIdsBindData &bind) {
	auto since = ParseIsoUtc(bind.since, "since");
	auto until = ParseIsoUtc(bind.until, "until");
	std::set<string> seen; // boundary dedup across latestDateCovered resumes
	for (int loop = 0; loop < kGetDeletedMaxLoops && since < until; loop++) {
		string path = "/services/data/" + g.api_version + "/sobjects/" + bind.object +
		              "/deleted/?start=" + ToIsoUtc(since) + "&end=" + ToIsoUtc(until);
		string body = g.session->AuthorizedGet(path);
		for (auto &rec : sfjson::GetObjectArray(body, "deletedRecords")) {
			string id = sfjson::GetString(rec, "id");
			// Boundary dedup: a resume slice can re-report a record already
			// emitted when latestDateCovered lands between slices.
			if (seen.count(id)) {
				continue;
			}
			seen.insert(id);
			g.rows.emplace_back(std::move(id), NormalizeDeletedDate(sfjson::GetString(rec, "deletedDate")));
		}
		// Resume from the covered frontier (falls back to `until` when absent).
		string covered = sfjson::GetString(body, "latestDateCovered");
		if (covered.empty()) {
			break;
		}
		auto covered_tp = ParseIsoUtc(NormalizeDeletedDate(covered) + "Z", "latestDateCovered");
		if (covered_tp <= since) {
			break; // no forward progress
		}
		if (covered_tp >= until) {
			break; // window fully covered
		}
		since = covered_tp;
	}
}

// queryAll sweep (opt-in source): FetchPage-based, so > 2,000 ids paginate via
// nextRecordsUrl (the v0.19.0 release truncated here — fixed).
void LoadViaQueryAll(DeletedIdsGlobalState &g, const DeletedIdsBindData &bind) {
	string soql = "SELECT Id FROM " + bind.object + " WHERE IsDeleted = true AND SystemModstamp >= " + bind.since;
	string next = "/services/data/" + g.api_version + "/queryAll/?q=" + StringUtil::URLEncode(soql);
	for (idx_t page = 0; page < kQueryAllMaxPages; page++) {
		auto pg = g.session->FetchPage(next);
		for (auto &rec : pg.records) {
			g.rows.emplace_back(sfjson::GetString(rec, "Id"), "");
		}
		if (pg.done || pg.next_path.empty()) {
			return;
		}
		next = pg.next_path; // opaque nextRecordsUrl, used verbatim
	}
	throw IOException("salesforce_deleted_ids: queryAll sweep exceeded %llu pages; "
	                  "narrow the window (source := 'getDeleted' with shorter since/until).",
	                  (unsigned long long)kQueryAllMaxPages);
}

void DeletedIdsFunction(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind = data.bind_data->Cast<DeletedIdsBindData>();
	auto &g = data.global_state->Cast<DeletedIdsGlobalState>();

	if (!g.loaded) {
		g.loaded = true;
		g.source = bind.source;
		try {
			if (bind.source == "getDeleted") {
				LoadViaGetDeleted(g, bind);
			} else {
				LoadViaQueryAll(g, bind);
			}
		} catch (std::exception &ex) {
			g.failed = true;
			g.error = ex.what();
		}
		if (g.failed) {
			throw IOException("salesforce_deleted_ids failed for '%s' (%s %s -> %s): %s", bind.object, bind.source,
			                  bind.since, bind.until, g.error);
		}
	}

	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE && g.cursor < g.rows.size()) {
		auto &id = g.rows[g.cursor].first;
		auto &deleted_date = g.rows[g.cursor].second;
		FlatVector::GetData<string_t>(output.data[0])[row] = StringVector::AddString(output.data[0], id);
		if (deleted_date.empty()) {
			FlatVector::SetNull(output.data[1], row, true);
		} else {
			FlatVector::GetData<string_t>(output.data[1])[row] = StringVector::AddString(output.data[1], deleted_date);
		}
		FlatVector::GetData<string_t>(output.data[2])[row] = StringVector::AddString(output.data[2], g.source);
		g.cursor++;
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
	fn.named_parameters["source"] = LogicalType::VARCHAR;
	return fn;
}

} // namespace duckdb

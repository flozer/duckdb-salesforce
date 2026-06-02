// Quota governor (#v0.4). Pre-flight gate on Bulk query-job starts so a large
// extraction cannot silently exhaust the org's daily API allocation. REST scans
// are intentionally NOT gated (keeps interactive flows cheap and never blocks a
// small query). Errors are clear and secret-free: never a bearer/body/secret.

#include "salesforce_quota.hpp"
#include "salesforce_session.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace duckdb {

namespace {

bool SettingBool(ClientContext &ctx, const char *key, bool dflt) {
    Value v;
    if (ctx.TryGetCurrentSetting(key, v) && !v.IsNull()) {
        return v.GetValue<bool>();
    }
    return dflt;
}

int64_t SettingInt(ClientContext &ctx, const char *key, int64_t dflt) {
    Value v;
    if (ctx.TryGetCurrentSetting(key, v) && !v.IsNull()) {
        return v.GetValue<int64_t>();
    }
    return dflt;
}

// --- per-instance_url in-memory cache (no disk) ------------------------------
struct CacheEntry {
    SalesforceQuotaSnapshot snap;
    std::chrono::steady_clock::time_point at;
};
std::mutex g_cache_lock;
std::unordered_map<string, CacheEntry> g_cache;

// --- last-decision diagnostic ------------------------------------------------
std::mutex g_last_lock;
string g_last_limit_name;
int64_t g_last_max = -1, g_last_remaining = -1, g_last_threshold = -1;
bool g_last_allowed = true;
string g_last_reason;

void RecordDecision(const string &limit_name, int64_t mx, int64_t rem, int64_t thr, bool allowed,
                    const string &reason) {
    std::lock_guard<std::mutex> g(g_last_lock);
    g_last_limit_name = limit_name;
    g_last_max = mx;
    g_last_remaining = rem;
    g_last_threshold = thr;
    g_last_allowed = allowed;
    g_last_reason = reason;
}

struct LastQuotaGlobalState : public GlobalTableFunctionState {
    bool emitted = false;
    idx_t MaxThreads() const override {
        return 1;
    }
};

unique_ptr<FunctionData> LastQuotaBind(ClientContext &, TableFunctionBindInput &,
                                       vector<LogicalType> &return_types, vector<string> &names) {
    names = {"limit_name", "max", "remaining", "threshold", "allowed", "reason"};
    return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
                    LogicalType::BIGINT, LogicalType::BOOLEAN, LogicalType::VARCHAR};
    return nullptr;
}

unique_ptr<GlobalTableFunctionState> LastQuotaInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<LastQuotaGlobalState>();
}

void EmitInt(Vector &vec, int64_t v) {
    if (v < 0) {
        FlatVector::SetNull(vec, 0, true);
    } else {
        FlatVector::GetData<int64_t>(vec)[0] = v;
    }
}

void LastQuotaFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &gstate = data.global_state->Cast<LastQuotaGlobalState>();
    if (gstate.emitted) {
        output.SetCardinality(0);
        return;
    }
    string name, reason;
    int64_t mx, rem, thr;
    bool allowed;
    {
        std::lock_guard<std::mutex> g(g_last_lock);
        name = g_last_limit_name;
        mx = g_last_max;
        rem = g_last_remaining;
        thr = g_last_threshold;
        allowed = g_last_allowed;
        reason = g_last_reason;
    }
    FlatVector::GetData<string_t>(output.data[0])[0] = StringVector::AddString(output.data[0], name);
    EmitInt(output.data[1], mx);
    EmitInt(output.data[2], rem);
    EmitInt(output.data[3], thr);
    FlatVector::GetData<bool>(output.data[4])[0] = allowed;
    FlatVector::GetData<string_t>(output.data[5])[0] =
        StringVector::AddString(output.data[5], reason);
    gstate.emitted = true;
    output.SetCardinality(1);
}

} // namespace

void QuotaGuardBulkStart(ClientContext &context, SalesforceSession &session,
                         const string &instance_url) {
    if (!SettingBool(context, "sf_quota_enabled", true)) {
        RecordDecision("", -1, -1, -1, true, "disabled: governor off -> allowed (no /limits)");
        return; // skip /limits entirely
    }

    const bool enforce = SettingBool(context, "sf_quota_enforce", true);
    const bool fail_open = SettingBool(context, "sf_quota_fail_open", true);
    const int64_t reserve_pct = SettingInt(context, "sf_quota_reserve_pct", 10);
    const int64_t min_remaining = SettingInt(context, "sf_quota_min_remaining", 1000);
    const int64_t ttl = SettingInt(context, "sf_quota_cache_seconds", 60);

    // Cache lookup (per instance_url, in memory, TTL-bounded).
    SalesforceQuotaSnapshot snap;
    bool from_cache = false;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> g(g_cache_lock);
        auto it = g_cache.find(instance_url);
        if (it != g_cache.end()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.at).count();
            if (ttl > 0 && age < ttl) {
                snap = it->second.snap;
                from_cache = true;
            }
        }
    }
    if (!from_cache) {
        snap = session.QueryLimits();
        if (ttl > 0 && snap.available) {
            std::lock_guard<std::mutex> g(g_cache_lock);
            g_cache[instance_url] = CacheEntry{snap, now};
        }
    }

    const char *src = from_cache ? " (cached)" : "";

    if (!snap.available) {
        if (fail_open) {
            RecordDecision("", -1, -1, -1, true,
                           string("limits unavailable -> allowed (fail-open)") + src);
            return;
        }
        RecordDecision("", -1, -1, -1, false,
                       string("limits unavailable -> blocked (fail-closed)") + src);
        throw IOException(
            "salesforce quota guard: /limits is unavailable and sf_quota_fail_open=false — "
            "refusing to start a Bulk job. Set sf_quota_fail_open=true to proceed, or "
            "sf_quota_enabled=false to disable the governor.");
    }

    // Threshold on DailyApiRequests = max(min_remaining, reserve_pct% of Max).
    int64_t reserve = (reserve_pct > 0) ? (snap.api_max * reserve_pct) / 100 : 0;
    int64_t threshold = std::max(min_remaining, reserve);
    bool ok = snap.api_remaining > threshold;

    // If the org reports a Bulk-query-jobs daily cap, honour it too.
    string bulk_note;
    if (snap.bulk_remaining == 0) {
        ok = false;
        bulk_note = "; DailyBulkV2QueryJobs exhausted";
    }

    if (ok) {
        RecordDecision("DailyApiRequests", snap.api_max, snap.api_remaining, threshold, true,
                       StringUtil::Format("allowed: Remaining %lld > threshold %lld%s",
                                          (long long)snap.api_remaining, (long long)threshold, src));
        return;
    }

    string reason = StringUtil::Format(
        "DailyApiRequests Remaining %lld <= threshold %lld (reserve %lld%% of %lld, min %lld)%s%s",
        (long long)snap.api_remaining, (long long)threshold, (long long)reserve_pct,
        (long long)snap.api_max, (long long)min_remaining, bulk_note.c_str(), src);

    if (!enforce) {
        RecordDecision("DailyApiRequests", snap.api_max, snap.api_remaining, threshold, true,
                       "WARN (sf_quota_enforce=false), proceeding: " + reason);
        return; // warn-only: do not block
    }

    RecordDecision("DailyApiRequests", snap.api_max, snap.api_remaining, threshold, false, reason);
    throw IOException(
        "salesforce quota guard: %s — refusing to start a Bulk job. SET sf_quota_enforce=false "
        "to warn-only, sf_quota_enabled=false to disable, raise sf_quota_reserve_pct/"
        "sf_quota_min_remaining, or wait for the daily reset.",
        reason);
}

TableFunction GetSalesforceLastQuotaFunction() {
    return TableFunction("salesforce_last_quota", {}, LastQuotaFunction, LastQuotaBind,
                         LastQuotaInit);
}

} // namespace duckdb

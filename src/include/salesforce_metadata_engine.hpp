#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"

namespace duckdb {

class ClientContext;

// Metadata Engine v2 (ROADMAP v1.6 §17) — a shared, read-only, per-catalog
// metadata cache. One instance per attached SalesforceCatalog (per org/ATTACH);
// never shared across orgs. It de-duplicates Describe Global + per-object
// Describe calls and is the single metadata path for Report Bridge and the
// metadata diagnostic function. In-memory only; no Tooling/Metadata API; no
// persistence. Invalidated by salesforce_refresh_metadata().
//
// All accessors build an HTTP client from the catalog context on a miss and
// memoize the result; a hit issues no Salesforce call.
class SalesforceMetadataEngine {
public:
    SalesforceMetadataEngine(SalesforceConfig config, SalesforceTokenSet token)
        : config_(std::move(config)), token_(std::move(token)) {}

    // Queryable object names (one Describe Global, memoized).
    const vector<string> &GetGlobalObjects(ClientContext &context);

    // Per-object REST Describe (memoized by lower(object)).
    const SalesforceDescribe &GetObjectDescribe(ClientContext &context, const string &object);

    // Membership in GetGlobalObjects(), case-insensitive.
    bool IsQueryable(ClientContext &context, const string &object);

    // Resolve a report token to a real field API name on `object` (as-is /
    // builtin token map / UPPER_SNAKE->PascalCase), confirmed against the
    // describe. out_filterable reflects the resolved field. False if unresolved.
    bool ResolveField(ClientContext &context, const string &object, const string &token,
                      string &out_realname, bool &out_filterable);

    // Resolve a single-hop relationship: `relationship_name` must exist on
    // `object` with exactly one referenceTo (non-polymorphic) whose target is
    // queryable. out_target = the related object API name. False otherwise.
    bool ResolveRelationship(ClientContext &context, const string &object,
                             const string &relationship_name, string &out_target);

    // Invalidation. Refresh(object) drops one object's describe; RefreshAll drops
    // the global list + every object describe. Mirrors salesforce_refresh_metadata.
    void Refresh(const string &object);
    void RefreshAll();

private:
    SalesforceConfig config_;
    SalesforceTokenSet token_;
    bool global_loaded_ = false;
    vector<string> global_objects_;
    std::unordered_map<string, SalesforceDescribe> describe_; // lower(object) -> describe
};

// Resolve an attached Salesforce catalog by ATTACH alias and return its shared
// metadata engine (per catalog/org). Throws a clear, secret-free BinderException
// if `alias` is not an attached Salesforce catalog.
SalesforceMetadataEngine &GetSalesforceCatalogMetadataEngine(ClientContext &context,
                                                             const string &alias);

// TEST/foundation harness (Phase A): salesforce_metadata_probe(catalog, object)
// exercises the engine so offline tests can assert cache hit / invalidation /
// per-catalog isolation via the describe-call counters. Not a user-facing API.
TableFunction GetSalesforceMetadataProbeFunction();

} // namespace duckdb

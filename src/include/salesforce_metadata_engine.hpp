#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_config.hpp"
#include "salesforce_describe.hpp"

namespace duckdb {

class ClientContext;
class SalesforceHttpClient;
class SalesforceSession;
struct SalesforceObjectInfo;

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
    SalesforceMetadataEngine(SalesforceConfig config, SalesforceTokenSet token);
    ~SalesforceMetadataEngine(); // both defined in .cpp (unique_ptr to incomplete types)

    // Queryable object names (one Describe Global, memoized). Derived from the
    // full global_infos_ cache (queryable==true only) — same result as before.
    const vector<string> &GetGlobalObjects(ClientContext &context);

    // Every global sObject with its queryable flag (one Describe Global, shared
    // with GetGlobalObjects). For the salesforce_metadata_objects diagnostic.
    const vector<SalesforceObjectInfo> &GetGlobalObjectInfos(ClientContext &context);

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
    // queryable. out_target = the related object API name; out_real_name = the
    // describe's canonical relationshipName (for emitting Rel.Field). False
    // otherwise.
    bool ResolveRelationship(ClientContext &context, const string &object,
                             const string &relationship_name, string &out_target,
                             string &out_real_name);

    // Invalidation. Refresh(object) drops one object's describe; RefreshAll drops
    // the global list + every object describe. Mirrors salesforce_refresh_metadata.
    void Refresh(const string &object);
    void RefreshAll();

private:
    // One lazily-built HTTP client + session for the engine's lifetime, so a
    // sequence of Describe calls behaves like a single session (and the offline
    // mock's '|~|' body sequencing advances correctly).
    SalesforceSession &Session(ClientContext &context);

    SalesforceConfig config_;
    SalesforceTokenSet token_;
    unique_ptr<SalesforceHttpClient> client_;
    unique_ptr<SalesforceSession> session_;
    // Single source of truth for the global describe. global_objects_ (queryable
    // names) is derived from global_infos_ on load.
    void EnsureGlobalLoaded(ClientContext &context);
    bool global_loaded_ = false;
    vector<SalesforceObjectInfo> global_infos_;
    vector<string> global_objects_;
    std::unordered_map<string, SalesforceDescribe> describe_; // lower(object) -> describe
};

// Resolve an attached Salesforce catalog by ATTACH alias and return its shared
// metadata engine (per catalog/org). Throws a clear, secret-free BinderException
// if `alias` is not an attached Salesforce catalog.
SalesforceMetadataEngine &GetSalesforceCatalogMetadataEngine(ClientContext &context,
                                                             const string &alias);

// salesforce_metadata_fields(catalog, object_name) — read-only diagnostic; one
// row per field, sourced through the shared engine (shares the metadata cache).
// Columns: object_name, field_name, type, filterable, sortable,
// relationship_name, reference_to LIST, picklist_values LIST.
TableFunction GetSalesforceMetadataFieldsFunction();

// salesforce_metadata_objects(catalog) — read-only diagnostic; one row per
// global sObject (object_name, queryable), sourced through the shared engine.
TableFunction GetSalesforceMetadataObjectsFunction();

// salesforce_query_explain() — read-only, last-scan diagnostic (#v1.6). One row
// per projected field / conjunctive filter of the most recent catalog scan,
// annotated via the shared Metadata Engine (resolved/filterable/sortable/
// relationship/referenceTo) with a closed reason set. Diagnostic-only: never
// affects scan execution or salesforce_query_cost().
TableFunction GetSalesforceQueryExplainFunction();

} // namespace duckdb

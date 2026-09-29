#define DUCKDB_EXTENSION_MAIN

#include "salesforce_extension.hpp"
#include "salesforce_storage.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_query.hpp"
#include "salesforce_metadata_engine.hpp"
#include "salesforce_report.hpp"
#include "salesforce_value.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_quota.hpp"
#include "salesforce_diag.hpp"
#include "salesforce_reldiag.hpp"
#include "salesforce_aggregate.hpp"
#include "salesforce_agg_optimizer.hpp"
#include "salesforce_metadata.hpp"

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

namespace {

// Register a function with a FunctionDescription attached so it documents
// itself through duckdb_functions(): real parameter names, a one-sentence
// description, a runnable example, and categories (#66). on_conflict must
// mirror the bare RegisterFunction overloads (ALTER), because the
// Create*FunctionInfo default is ERROR_ON_CONFLICT and would break reload.
void RegisterDescribed(ExtensionLoader &loader, TableFunction function, vector<string> parameter_names,
                       string description, vector<string> examples, vector<string> categories) {
	CreateTableFunctionInfo info(std::move(function));
	FunctionDescription desc;
	desc.parameter_names = std::move(parameter_names);
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.categories = std::move(categories);
	info.descriptions.push_back(std::move(desc));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	loader.RegisterFunction(std::move(info));
}

void RegisterDescribed(ExtensionLoader &loader, ScalarFunction function, vector<string> parameter_names,
                       string description, vector<string> examples, vector<string> categories) {
	CreateScalarFunctionInfo info(std::move(function));
	FunctionDescription desc;
	desc.parameter_names = std::move(parameter_names);
	desc.description = std::move(description);
	desc.examples = std::move(examples);
	desc.categories = std::move(categories);
	info.descriptions.push_back(std::move(desc));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	loader.RegisterFunction(std::move(info));
}

} // namespace

static void LoadInternal(ExtensionLoader &loader) {
	// Scope the last-scan diagnostics (query_cost / query_explain) to this
	// database instance: clear any stale snapshot so query_explain() returns
	// zero rows until a real scan runs (#v1.6 — no fabricated meta rows).
	DiagReset();

	// Register the StorageExtension so DuckDB recognises
	//   ATTACH 'salesforce://…' AS sf (TYPE salesforce);
	// as a known storage type. v0.1 scaffold: the attach callback throws a
	// clear NotImplementedException (see salesforce_storage.cpp).
	//
	// StorageExtension::Register matches the registration path used by
	// duckdb-firebird. Verified against the pinned DuckDB v1.5.3 release:
	// ATTACH dispatch requires BOTH attach and create_transaction_manager to
	// be non-null (see salesforce_storage.cpp).
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	auto storage_ext = GetSalesforceStorageExtension();
	StorageExtension::Register(config, "salesforce", shared_ptr<StorageExtension>(storage_ext.release()));

	// Transparent aggregate pushdown (P1.2a): post-optimizer pass rewriting
	// Aggregate(COUNT(col)) directly over a salesforce_scan into ONE
	// server-side COUNT query (single-row result), guarded + kill-switchable
	// via sf_aggregate_pushdown below. Spec:
	// docs/superpowers/specs/2026-09-29-aggregate-pushdown-optimizer-extension-feasibility.md.
	OptimizerExtension agg_pushdown;
	agg_pushdown.optimize_function = SalesforceAggregatePushdownOptimize;
	OptimizerExtension::Register(config, std::move(agg_pushdown));
	// Kill-switch for the pass above. Default ON (same stance as the
	// transparent COUNT(*) pushdown); false restores the row-scan + local
	// aggregation plan shape.
	config.AddExtensionOption("sf_aggregate_pushdown",
	                          "Transparent COUNT(field) pushdown: when a no-group COUNT query runs "
	                          "directly on an attached sObject with every filter already pushed to "
	                          "SOQL, run one server-side COUNT query instead of fetching rows "
	                          "(default true). false keeps the row-scan fallback.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));

	// salesforce_describe(object, client_id:=, client_secret:=, refresh_token:=,
	//   login_url:=, api_version:=) — introspect a single sObject's schema (#5).
	RegisterDescribed(loader, GetSalesforceDescribeFunction(), {"object"},
	                  "Fetches the REST describe metadata (fields, types, relationships) for a single "
	                  "Salesforce sObject without an ATTACH; credentials are passed as the named "
	                  "parameters client_id, client_secret, refresh_token, login_url and api_version.",
	                  {"SELECT * FROM salesforce_describe('Account', client_id := '<client_id>', "
	                   "client_secret := '<client_secret>', refresh_token := '<refresh_token>');"},
	                  {"metadata"});

	// salesforce_query(soql, client_id:=, ...) — paginated SOQL fetcher (#6),
	// returns raw JSON records. Typed scanning lands in #7/#8.
	RegisterDescribed(loader, GetSalesforceQueryFunction(), {"soql"},
	                  "Runs a read-only SOQL query against Salesforce and returns the raw JSON records "
	                  "with lazy pagination; credentials use the same named parameters as "
	                  "salesforce_describe().",
	                  {"SELECT * FROM salesforce_query('SELECT Id, Name FROM Account LIMIT 10', client_id "
	                   ":= '<client_id>', client_secret := '<client_secret>', refresh_token := "
	                   "'<refresh_token>');"},
	                  {"query"});
	RegisterDescribed(loader, GetSalesforceUrlEncodeFunction(), {"text"},
	                  "Percent-encodes a string for safe use as a literal inside a SOQL query or a "
	                  "Salesforce REST URL.",
	                  {"sf_url_encode('Acme & Sons')"}, {"utility"});

	// salesforce_decode(fields_json, records_json) — JSON record -> typed
	// DuckDB vectors (#7). Test/utility surface; the scan wires it in at #8.
	RegisterDescribed(loader, GetSalesforceDecodeFunction(), {"fields_json", "records_json"},
	                  "Decodes a fields-describe JSON array plus a records JSON array into typed DuckDB "
	                  "rows; utility surface of the scan's JSON-to-vector conversion.",
	                  {"SELECT * FROM salesforce_decode('[{\"name\":\"Name\",\"type\":\"string\"}]', "
	                   "'[{\"Name\":\"Acme\"}]');"},
	                  {"utility"});

	// salesforce_last_soql() — diagnostic: the SOQL the most recent scan
	// generated (projection + predicate pushdown). Used by tests.
	RegisterDescribed(loader, GetSalesforceLastSoqlFunction(), {},
	                  "Returns the SOQL (projection plus predicate pushdown) generated by the most "
	                  "recent salesforce scan; returns no rows until a scan has run.",
	                  {"SELECT * FROM salesforce_last_soql();"}, {"diagnostic"});

	// salesforce_last_scan_pages() — DEBUG/TEST ONLY: query pages the most
	// recent scan fetched (proves lazy pagination, #11). Not a public API.
	RegisterDescribed(loader, GetSalesforceLastScanPagesFunction(), {},
	                  "TEST ONLY: returns the number of query pages the most recent scan fetched, "
	                  "proving lazy pagination.",
	                  {"SELECT * FROM salesforce_last_scan_pages();"}, {"diagnostic", "test"});

	// salesforce_last_bulk_create_body() — DEBUG/TEST ONLY: JSON body of the
	// most recent Bulk job-create POST, so tests can assert the Bulk job carries
	// the same projection + predicate SOQL as REST (v0.3). Not a public API.
	RegisterDescribed(loader, GetSalesforceLastBulkCreateBodyFunction(), {},
	                  "TEST ONLY: returns the JSON body of the most recent Bulk API 2.0 job-create POST, "
	                  "showing the pushed SOQL.",
	                  {"SELECT * FROM salesforce_last_bulk_create_body();"}, {"diagnostic", "test"});

	// salesforce_last_transport() — DEBUG/TEST ONLY: transport the most recent
	// scan resolved to + probed est_rows + reason (proves 'auto' selection,
	// v0.3 §2). Also user-facing diagnostic for why REST vs Bulk was chosen.
	RegisterDescribed(loader, GetSalesforceLastTransportFunction(), {},
	                  "Returns the transport (rest or bulk) the most recent scan resolved to, with the "
	                  "row-count estimate and the reason for the choice.",
	                  {"SELECT * FROM salesforce_last_transport();"}, {"diagnostic"});

	// salesforce_last_quota() — DEBUG/diagnostic: the last quota-governor
	// decision (limit_name, max, remaining, threshold, allowed, reason). v0.4.
	RegisterDescribed(loader, GetSalesforceLastQuotaFunction(), {},
	                  "Returns the most recent quota-governor decision for Bulk job starts: limit name, "
	                  "max, remaining, threshold, whether the job was allowed, and why.",
	                  {"SELECT * FROM salesforce_last_quota();"}, {"diagnostic"});

	// salesforce_query_cost() — unified LAST-SCAN cost view (#v0.4 §4): SOQL,
	// transport, projection ratio, pushed/residual filter counts, pages, rows
	// delivered, quota, and short selectivity guidance. Aggregates the granular
	// salesforce_last_* diagnostics; does not replace them.
	RegisterDescribed(loader, GetSalesforceQueryCostFunction(), {},
	                  "Returns a unified cost view of the most recent scan: SOQL, transport, projection "
	                  "ratio, pushed and residual filter counts, pages, rows delivered, quota decision "
	                  "and Bulk poll count.",
	                  {"SELECT * FROM salesforce_query_cost();"}, {"diagnostic"});

	// Report Bridge (§16) — list report definitions.
	RegisterDescribed(loader, GetSalesforceReportsFunction(), {"catalog"},
	                  "Lists the Salesforce report definitions visible to the authenticated user, for "
	                  "Report Bridge discovery and validation.",
	                  {"SELECT * FROM salesforce_reports('sf');"}, {"report"});
	// Report Bridge (§16) Phase C — tabular report sample + diagnostics.
	RegisterDescribed(loader, GetSalesforceReportFunction(), {"catalog", "report_id"},
	                  "Runs a tabular Salesforce report synchronously and returns its fact rows plus run "
	                  "diagnostics; capped at 2,000 rows, for discovery and validation rather than "
	                  "large extraction.",
	                  {"SELECT * FROM salesforce_report('sf', '00O...');"}, {"report"});
	// Report Bridge (§16) Phase D — best-effort candidate SOQL reconstruction.
	RegisterDescribed(loader, GetSalesforceReportSoqlFunction(), {"catalog", "report_id"},
	                  "Returns a best-effort, describe-validated candidate SOQL translation of a report "
	                  "with explainability columns (translatable, translation_status, blocked_by, "
	                  "confidence); reports translatable = false instead of an unverified SOQL.",
	                  {"SELECT * FROM salesforce_report_soql('sf', '00O...');"}, {"report", "diagnostic"});
	// Metadata Engine v2 (§17) — read-only field metadata diagnostic.
	RegisterDescribed(loader, GetSalesforceMetadataFieldsFunction(), {"catalog", "object"},
	                  "Returns per-field schema metadata for an sObject in an attached salesforce "
	                  "catalog (name, type, capability flags, reference target, picklist values) from "
	                  "the shared read-only metadata cache.",
	                  {"SELECT field_name, type FROM salesforce_metadata_fields('sf', 'Account');"}, {"metadata"});
	RegisterDescribed(loader, GetSalesforceMetadataObjectsFunction(), {"catalog"},
	                  "Lists the sObjects visible to the authenticated user in an attached salesforce "
	                  "catalog, with queryable and other capability flags, from the shared read-only "
	                  "metadata cache.",
	                  {"SELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable;"}, {"metadata"});
	RegisterDescribed(loader, GetSalesforceRelationshipGraphFunction(), {"catalog", "object"},
	                  "Enumerates relationship edges reachable from an sObject with an explicit status "
	                  "per edge (resolved, polymorphic, self_reference, cyclic, ...); accepts an "
	                  "optional positional max_depth plus the named parameters include_children "
	                  "(default false) and direction ('parent', 'child' or 'both', default 'parent').",
	                  {"SELECT * FROM salesforce_relationship_graph('sf', 'Contact');"}, {"metadata", "diagnostic"});
	RegisterDescribed(loader, GetSalesforceQueryExplainFunction(), {},
	                  "Returns a field-by-field explanation of the most recent scan: which filters were "
	                  "pushed to SOQL versus evaluated as residuals, plus projection, relationship, "
	                  "count and transport decisions; diagnostic only, never changes scan behavior.",
	                  {"SELECT * FROM salesforce_query_explain();"}, {"diagnostic"});

	// salesforce_relationships() — LAST-RESOLUTION relationship diagnostics
	// (#v1.0): one `config` row (sf_relationships mode, effective depth,
	// expanded/skipped counts) plus one `relationship` row per reference field
	// considered (expanded with field_count, or skipped with a reason:
	// polymorphic / self_reference / cycle / name_collision /
	// parent_not_describable / no_fields / no_relationship_name). Explains
	// over-fetch and why a parent was or wasn't expanded. Read-only diagnostic.
	RegisterDescribed(loader, GetSalesforceRelationshipsFunction(), {},
	                  "Explains the most recent scan's relationship resolution: one config row (mode, "
	                  "effective depth, expanded/skipped counts) plus one row per reference field, "
	                  "expanded with field counts or skipped with a reason.",
	                  {"SELECT * FROM salesforce_relationships();"}, {"diagnostic"});

	// salesforce_aggregate(catalog, object, aggregates [, filter]) — explicit,
	// opt-in server-side SOQL aggregates (#v1.0): runs
	// SELECT <aggregates> FROM <object> [WHERE <filter>] over an attached
	// catalog and returns one row, one VARCHAR column per aggregate term. Not
	// transparent pushdown — the user chooses it. No optimizer / plan rewrite.
	RegisterDescribed(loader, GetSalesforceAggregateFunction(), {"catalog", "object", "aggregates"},
	                  "Runs an explicit server-side SOQL aggregate over an attached catalog - SELECT "
	                  "<aggregates> FROM <object> [WHERE <filter>] [GROUP BY <group_by>] - with optional "
	                  "positional filter and group_by arguments (3 to 5 total) and one VARCHAR column "
	                  "per aggregate term; opt-in, not transparent pushdown.",
	                  {"SELECT * FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', "
	                   "'IsDeleted = false', 'Industry');"},
	                  {"aggregate"});

	// salesforce_refresh_metadata(catalog [, object]) — manual metadata-cache
	// refresh (#v1.3 §10): clears the attached catalog's in-memory schema +
	// object-listing cache so the next reference re-describes. Empty object =
	// global; a named object clears only that object. No data/disk cache.
	RegisterDescribed(loader, GetSalesforceRefreshMetadataFunction(), {"catalog"},
	                  "Invalidates the in-memory metadata cache of an attached catalog - the whole "
	                  "catalog when no object is given, only the named object otherwise - so the next "
	                  "reference re-describes.",
	                  {"SELECT * FROM salesforce_refresh_metadata('sf', 'Account');"}, {"metadata"});

	// salesforce_picklist_values(catalog, object, field) +
	// salesforce_record_types(catalog, object) — read-only metadata enrichment
	// (#v1.3 §14) parsed from the cached REST describe. Not the Metadata API.
	RegisterDescribed(loader, GetSalesforcePicklistValuesFunction(), {"catalog", "object", "field"},
	                  "Returns the picklist entries of a field from the cached REST describe with value, "
	                  "label and active flag; read-only metadata, not the Metadata API.",
	                  {"SELECT value, label FROM salesforce_picklist_values('sf', 'Account', 'Industry') "
	                   "WHERE active;"},
	                  {"metadata"});
	RegisterDescribed(loader, GetSalesforceRecordTypesFunction(), {"catalog", "object"},
	                  "Returns the record types of an sObject from the cached REST describe: "
	                  "developer_name, label, record_type_id, active and is_default.",
	                  {"SELECT developer_name, label, is_default FROM salesforce_record_types('sf', "
	                   "'Account');"},
	                  {"metadata"});

	// salesforce_describe_calls() — DEBUG/TEST ONLY: sObject describes the
	// attached catalog issued since ATTACH (proves the metadata cache, #12).
	RegisterDescribed(loader, GetSalesforceDescribeCallsFunction(), {},
	                  "TEST ONLY: returns the number of sObject describes issued since ATTACH, proving "
	                  "the metadata cache is reused.",
	                  {"SELECT * FROM salesforce_describe_calls();"}, {"diagnostic", "test"});

	// salesforce_global_describe_calls() — DEBUG/TEST ONLY: global describes
	// (GET /sobjects) since ATTACH (proves object-list discovery, #14).
	RegisterDescribed(loader, GetSalesforceGlobalDescribeCallsFunction(), {},
	                  "TEST ONLY: returns the number of global describe (GET /sobjects) calls issued "
	                  "since ATTACH, proving object-list discovery caching.",
	                  {"SELECT * FROM salesforce_global_describe_calls();"}, {"diagnostic", "test"});

	// salesforce_tooling_calls() — DEBUG/TEST ONLY: Tooling API schema queries
	// since ATTACH (proves fast-schema use + batching, #v0.6 §6).
	RegisterDescribed(loader, GetSalesforceToolingCallsFunction(), {},
	                  "TEST ONLY: returns the number of Tooling API schema queries issued since ATTACH, "
	                  "proving fast-schema batching (sf_schema_source = 'tooling').",
	                  {"SELECT * FROM salesforce_tooling_calls();"}, {"diagnostic", "test"});

	// Test-only hooks for the OAuth exchange (#3). When sf_mock_token_status is
	// non-zero, ATTACH uses a mock HTTP client returning that status and
	// sf_mock_token_body instead of the live transport, so sqllogictest can
	// exercise token exchange without contacting Salesforce. These inject a
	// canned RESPONSE only and never touch request secrets. Default (0)
	// disables the hook.
	config.AddExtensionOption("sf_mock_token_status",
	                          "TEST ONLY. HTTP status for a mocked Salesforce token-endpoint "
	                          "response. 0 disables the mock and uses the live transport (default).",
	                          LogicalType::BIGINT, Value::BIGINT(0));
	config.AddExtensionOption("sf_mock_token_body", "TEST ONLY. Response body paired with sf_mock_token_status.",
	                          LogicalType::VARCHAR, Value(""));
	// Mocked authenticated GETs are routed by URL into two scripted sequences:
	// describe (.../describe) and query (.../query). Each is a comma-separated
	// status list + '|~|'-separated bodies; the last entry repeats. Active only
	// when sf_mock_token_status != 0.
	config.AddExtensionOption("sf_mock_describe_status",
	                          "TEST ONLY. Statuses for mocked describe GETs (e.g. '200', '401,200').",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_describe_body", "TEST ONLY. Bodies for mocked describe GETs ('|~|'-separated).",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("sf_mock_query_status", "TEST ONLY. Statuses for mocked query GETs (e.g. '200,200').",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_query_body", "TEST ONLY. Bodies for mocked query GET pages ('|~|'-separated).",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("sf_mock_sobjects_status",
	                          "TEST ONLY. Statuses for mocked global describe (GET /sobjects).", LogicalType::VARCHAR,
	                          Value("200"));
	config.AddExtensionOption("sf_mock_sobjects_body", "TEST ONLY. Body for mocked global describe (GET /sobjects).",
	                          LogicalType::VARCHAR, Value(""));
	// Mocked COUNT() probe (auto-transport selection, §2): a GET whose SOQL
	// contains COUNT is routed here instead of the data-query sequence.
	config.AddExtensionOption("sf_mock_count_status", "TEST ONLY. Statuses for the mocked COUNT() probe GET.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_count_body", "TEST ONLY. Body for the mocked COUNT() probe (reads totalSize).",
	                          LogicalType::VARCHAR, Value("{\"totalSize\":0,\"done\":true,\"records\":[]}"));
	// Mocked /limits for the quota governor (§v0.4). Default is a healthy org so
	// existing Bulk tests pass unchanged.
	config.AddExtensionOption("sf_mock_limits_status", "TEST ONLY. Statuses for the mocked GET /limits.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_limits_body", "TEST ONLY. Body for the mocked GET /limits.",
	                          LogicalType::VARCHAR,
	                          Value("{\"DailyApiRequests\":{\"Max\":100000,\"Remaining\":99000}}"));

	// Transport for catalog scans: 'rest' (default, lazy REST /query), 'bulk'
	// (Bulk API 2.0 query path), or 'auto' (probe the row count and pick by
	// sf_auto_bulk_threshold). #v0.3. Default stays 'rest' to preserve the
	// interactive experience; 'auto' is opt-in; 'bulk' forces Bulk.
	config.AddExtensionOption("sf_force_transport",
	                          "Scan transport: 'rest' (default), 'bulk' (Bulk API 2.0), or 'auto' "
	                          "(choose by row-count probe). 'bulk' is for large extractions / CREATE "
	                          "TABLE AS / COPY; same SOQL (projection + predicate pushdown) either way.",
	                          LogicalType::VARCHAR, Value("rest"));
	// 'auto' tuning. The threshold is the row estimate above which 'auto' picks
	// Bulk. The probe is a single COUNT() REST call (zero row egress); disabling
	// it makes 'auto' always resolve to REST.
	config.AddExtensionOption("sf_auto_bulk_threshold",
	                          "For sf_force_transport='auto': estimated row count above which Bulk is "
	                          "chosen over REST (default 50000).",
	                          LogicalType::BIGINT, Value::BIGINT(50000));
	config.AddExtensionOption("sf_auto_probe",
	                          "For sf_force_transport='auto': run the COUNT() row-count probe (default "
	                          "true). When false, 'auto' always resolves to REST.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));

	// Quota governor (#v0.4). Gates Bulk job STARTS on the org's REST /limits;
	// REST scans are never preflight-gated. enabled=false skips /limits entirely;
	// enforce=false consults+reports but never blocks (warn); fail_open governs
	// behaviour when /limits is unavailable.
	config.AddExtensionOption("sf_quota_enabled",
	                          "Quota governor: gate Bulk job starts on the org's API quota "
	                          "(default true). false skips /limits and never blocks.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("sf_quota_enforce",
	                          "Quota governor: block when below reserve (default true). false = "
	                          "consult /limits and report, but proceed (warn-only).",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("sf_quota_fail_open",
	                          "Quota governor: when /limits is unavailable, allow the Bulk job "
	                          "(default true). false blocks with a clear error.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("sf_quota_reserve_pct",
	                          "Quota governor: keep this %% of DailyApiRequests.Max in reserve "
	                          "(default 10).",
	                          LogicalType::BIGINT, Value::BIGINT(10));
	config.AddExtensionOption("sf_quota_min_remaining",
	                          "Quota governor: absolute floor of remaining DailyApiRequests below "
	                          "which Bulk is refused (default 1000).",
	                          LogicalType::BIGINT, Value::BIGINT(1000));
	config.AddExtensionOption("sf_quota_cache_seconds",
	                          "Quota governor: in-memory TTL for a cached /limits snapshot, per "
	                          "instance_url (default 60; 0 disables caching).",
	                          LogicalType::BIGINT, Value::BIGINT(60));

	// Schema discovery source (#v0.6 §6): 'describe' (default, REST sObject
	// describe — authoritative) or 'tooling' (fast, batched Tooling API
	// FieldDefinition with per-object REST fallback on error/absent/unmapped).
	config.AddExtensionOption("sf_schema_source",
	                          "Schema discovery: 'describe' (default, REST, authoritative) or 'tooling' "
	                          "(fast batched Tooling API FieldDefinition; falls back to REST describe "
	                          "per object on error/absent/ambiguous type; coarser types; fields default "
	                          "non-filterable unless Tooling marks them filterable).",
	                          LogicalType::VARCHAR, Value("describe"));
	// Read mode (#v0.9 §1): 'query' (default) or 'queryAll'. queryAll also
	// returns archived + soft-deleted (IsDeleted=true) records, via the REST
	// /queryAll endpoint and Bulk operation "queryAll". Applies to the scan +
	// its COUNT()/MIN-MAX probes; the salesforce_query() utility stays query-only.
	config.AddExtensionOption("sf_query_mode",
	                          "Read mode: 'query' (default) or 'queryAll' (also returns archived + "
	                          "soft-deleted records). Affects the scan (REST + Bulk) and its probes.",
	                          LogicalType::VARCHAR, Value("query"));

	// Parent relationship expansion (#v0.6 §7). 'off' (default) leaves the
	// schema flat (no behaviour change); 'parent' exposes each single-target
	// parent relationship as a STRUCT column (e.g. Account on sf.Contact ->
	// SELECT Account.Name). Describe-source only; depth 1; polymorphic skipped.
	config.AddExtensionOption("sf_relationships",
	                          "Parent relationship traversal: 'off' (default) or 'parent' (expose each "
	                          "single-target parent as a STRUCT column, e.g. SELECT Account.Name FROM "
	                          "sf.Contact). Polymorphic/child relationships not expanded.",
	                          LogicalType::VARCHAR, Value("off"));
	// Depth of parent traversal when sf_relationships='parent' (#v1.0): 1
	// (default, parent only) or 2 (+ grandparent, e.g. Account.Owner.Name as a
	// nested STRUCT). Capped at 2; single-target only at each hop.
	config.AddExtensionOption("sf_relationship_depth",
	                          "Parent traversal depth when sf_relationships='parent': 1 (default, "
	                          "parent only) or 2 (also grandparent, nested STRUCT). Capped at 2.",
	                          LogicalType::BIGINT, Value::BIGINT(1));

	// Mocked Tooling query (§v0.6). GET .../tooling/query -> this sequence.
	config.AddExtensionOption("sf_mock_tooling_status", "TEST ONLY. Statuses for the mocked GET /tooling/query.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_tooling_body",
	                          "TEST ONLY. Body/pages for the mocked GET /tooling/query ('|~|').", LogicalType::VARCHAR,
	                          Value(""));
	// TEST ONLY (#v1.0 Auth UX). Overrides env-var lookup for auth_source=
	// 'env'/'sfdx_url' so the offline suite can exercise them without touching
	// the OS environment. Format: "NAME=value;NAME2=value2". Empty = use real env.
	config.AddExtensionOption("sf_mock_env",
	                          "TEST ONLY. Override environment-variable lookup for auth_source "
	                          "env/sfdx_url ('NAME=value;...'). Empty uses the real OS environment.",
	                          LogicalType::VARCHAR, Value(""));

	// Mocked GET /queryAll (#v0.9 §1) — distinct from /query so tests prove the
	// endpoint actually changes under sf_query_mode='queryAll'.
	config.AddExtensionOption("sf_mock_queryall_status", "TEST ONLY. Statuses for the mocked GET /queryAll.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_queryall_body", "TEST ONLY. Body/pages for the mocked GET /queryAll ('|~|').",
	                          LogicalType::VARCHAR, Value(""));

	// PK chunking for Bulk extraction (#v0.7 §9, cut 1 = sequential). 1 (default)
	// = no chunking. >1 splits the scan into N disjoint Id ranges (MIN/MAX(Id)
	// probe + uniform lexical split), each run as its own Bulk job, streamed
	// sequentially. Bulk-only (REST ignores it); capped at 8.
	config.AddExtensionOption("sf_bulk_chunks",
	                          "Bulk PK chunking: split a Bulk scan into N disjoint Id ranges (1 = off, "
	                          "default; capped at 8). Sequential in this cut. Bulk transport only.",
	                          LogicalType::BIGINT, Value::BIGINT(1));

	// Bulk backfill guardrails (ROADMAP §15). Poll budget bounds how long
	// BulkStartJob waits for a job to finish before failing fast; raise it for a
	// large backfill that legitimately needs more polls. require_predicate is an
	// opt-in guard that rejects a full-object Bulk read (no pushed predicate).
	config.AddExtensionOption("sf_bulk_poll_budget",
	                          "Max Bulk job-status polls before failing fast (default 600, ~250ms "
	                          "each). Raise for a large backfill; surfaced as salesforce_query_cost()"
	                          ".bulk_polls.",
	                          LogicalType::BIGINT, Value::BIGINT(600));
	config.AddExtensionOption("sf_bulk_require_predicate",
	                          "When true, a Bulk read with no predicate pushed to SOQL (full-object "
	                          "extraction) fails fast instead of running. Default false (guidance "
	                          "only). Use for planned large backfills to force a CreatedDate/"
	                          "SystemModstamp window.",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// Test-only Bulk mock hooks (active when sf_mock_token_status != 0).
	config.AddExtensionOption("sf_mock_bulk_create_status", "TEST ONLY. Bulk job-create HTTP status.",
	                          LogicalType::BIGINT, Value::BIGINT(200));
	config.AddExtensionOption("sf_mock_bulk_create_body", "TEST ONLY. Bulk job-create response body.",
	                          LogicalType::VARCHAR, Value("{\"id\":\"JOB1\",\"state\":\"UploadComplete\"}"));
	config.AddExtensionOption("sf_mock_bulk_status_code", "TEST ONLY. Bulk status HTTP status(es), CSV.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_bulk_status_body", "TEST ONLY. Bulk status body/bodies ('|~|' per poll).",
	                          LogicalType::VARCHAR, Value("{\"state\":\"JobComplete\"}"));
	config.AddExtensionOption("sf_mock_bulk_results_status", "TEST ONLY. Bulk results HTTP status(es), CSV.",
	                          LogicalType::VARCHAR, Value("200"));
	config.AddExtensionOption("sf_mock_bulk_results_body", "TEST ONLY. Bulk results CSV page(s) ('|~|' per page).",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("sf_mock_bulk_results_locator",
	                          "TEST ONLY. Sforce-Locator per results page (comma-separated; empty = last).",
	                          LogicalType::VARCHAR, Value(""));

	// Report Bridge analytics mock (§16). Run + describe sequences ('|~|').
	config.AddExtensionOption("sf_mock_report_status", "TEST ONLY. Analytics report-run HTTP status(es), CSV.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("sf_mock_report_body", "TEST ONLY. Analytics report-run JSON body/pages ('|~|').",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption("sf_mock_report_describe_status",
	                          "TEST ONLY. Analytics report /describe HTTP status(es), CSV.", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("sf_mock_report_describe_body",
	                          "TEST ONLY. Analytics report /describe JSON body ('|~|').", LogicalType::VARCHAR,
	                          Value(""));
	config.AddExtensionOption("sf_mock_report_token_map",
	                          "TEST ONLY. Inject report compound/address token mappings for the Report "
	                          "Bridge normalizer: 'Object:TOKEN=Field;Object:TOKEN2=Field2'. The real "
	                          "map is empty; this drives the mechanism in tests without hardcoded entries.",
	                          LogicalType::VARCHAR, Value(""));
}

void SalesforceExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string SalesforceExtension::Name() {
	return "salesforce";
}

std::string SalesforceExtension::Version() const {
#ifdef EXT_VERSION_SALESFORCE
	return EXT_VERSION_SALESFORCE;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

// --- C entry points -----------------------------------------------------------
// DuckDB ≥ 1.4 calls the *_duckdb_cpp_init symbol declared by this macro.
extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(salesforce, loader) {
	duckdb::LoadInternal(loader);
}

DUCKDB_EXTENSION_API const char *salesforce_version() {
	return duckdb::DuckDB::LibraryVersion();
}

} // extern "C"

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif

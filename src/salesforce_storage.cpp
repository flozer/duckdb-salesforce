// Salesforce ATTACH support — federated read-only catalog (issue #8).
//
// Mirrors duckdb-firebird's storage.cpp structure:
//   SalesforceCatalog            : Catalog
//   SalesforceSchemaEntry        : SchemaCatalogEntry  (one "main" schema)
//   SalesforceTableEntry         : TableCatalogEntry
//   SalesforceTransactionManager : TransactionManager  (no-op, read-only)
//
// ATTACH authenticates (OAuth refresh-token exchange, #3) and keeps the config
// + token in memory. Objects are resolved LAZILY by name: the first reference
// to sf.<Object> runs an sObject describe (#5) to build the DuckDB schema, then
// SELECT * scans via the query fetcher (#6) + JSON decoder (#7). Everything is
// read-only; all mutating catalog ops throw. No global object listing (v0.1
// limitation): SHOW TABLES reflects only objects resolved this session.

#include "salesforce_storage.hpp"
#include "salesforce_config.hpp"
#include "salesforce_auth.hpp"
#include "salesforce_http.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_scan.hpp"
#include "salesforce_session.hpp"
#include "salesforce_soql.hpp"
#include "salesforce_reldiag.hpp"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

static const char *const SALESFORCE_MAIN_SCHEMA = "main";

// Salesforce compound fields are not directly selectable in SOQL; their
// components appear as separate scalar fields, so we drop them from the table.
static bool IsQueryableField(const SalesforceField &f) {
    string t = StringUtil::Lower(f.sf_type);
    return t != "address" && t != "location";
}

// ---------------------------------------------------------------------------
//  SalesforceTableEntry
// ---------------------------------------------------------------------------

class SalesforceTableEntry final : public TableCatalogEntry {
public:
    SalesforceTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                         SalesforceConfig config, SalesforceTokenSet token,
                         vector<SalesforceField> fields)
        : TableCatalogEntry(catalog, schema, info), config_(std::move(config)),
          token_(std::move(token)), fields_(std::move(fields)) {
        for (auto &col : columns.Logical()) {
            column_names_.push_back(col.Name());
            column_types_.push_back(col.Type());
        }
    }

    unique_ptr<BaseStatistics> GetStatistics(ClientContext &, column_t) override {
        return nullptr;
    }

    TableStorageInfo GetStorageInfo(ClientContext &) override {
        return TableStorageInfo();
    }

    TableFunction GetScanFunction(ClientContext &, unique_ptr<FunctionData> &bind_data) override {
        auto data = make_uniq<SalesforceScanBindData>();
        data->config = config_;
        data->token = token_;
        data->object = name;
        data->fields = fields_;
        data->column_names = column_names_;
        data->column_types = column_types_;
        bind_data = std::move(data);
        return GetSalesforceScanFunction();
    }

private:
    SalesforceConfig config_;
    SalesforceTokenSet token_;
    vector<SalesforceField> fields_;
    vector<string> column_names_;
    vector<LogicalType> column_types_;
};

// ---------------------------------------------------------------------------
//  SalesforceSchemaEntry
// ---------------------------------------------------------------------------

class SalesforceSchemaEntry final : public SchemaCatalogEntry {
public:
    SalesforceSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, SalesforceConfig config,
                          SalesforceTokenSet token)
        : SchemaCatalogEntry(catalog, info), config_(std::move(config)),
          token_(std::move(token)) {}

    void Scan(ClientContext &context, CatalogType type,
              const std::function<void(CatalogEntry &)> &callback) override {
        if (type == CatalogType::TABLE_ENTRY) {
            EnsureObjectListLoaded(context); // one global describe (#14), cached
        }
        EmitResolved(type, callback);
    }
    void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override {
        // No context to fetch the object list here; emit whatever is cached.
        EmitResolved(type, callback);
    }

    optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction,
                                           const EntryLookupInfo &lookup_info) override {
        if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
            return nullptr;
        }
        if (!transaction.HasContext()) {
            return nullptr; // no context to issue the describe call
        }
        const string object = lookup_info.GetEntryName();
        const string key = StringUtil::Lower(object);

        std::lock_guard<std::mutex> g(lock_);
        auto it = tables_.find(key);
        if (it != tables_.end()) {
            return it->second.get();
        }

        auto client = BuildHttpClientForContext(transaction.GetContext());
        SalesforceSession session(config_, *client);
        session.SetToken(token_);

        // Schema source (#v0.6 §6): 'describe' (default, REST, authoritative) or
        // 'tooling' (fast batched FieldDefinition, with per-object REST fallback).
        string schema_source = "describe";
        Value sv;
        if (transaction.GetContext().TryGetCurrentSetting("sf_schema_source", sv) && !sv.IsNull()) {
            schema_source = StringUtil::Lower(sv.ToString());
        }

        if (schema_source == "tooling") {
            // Batch-warm: the requested object + any already-listed objects not
            // yet described, in one/few Tooling queries.
            vector<string> batch;
            batch.push_back(object);
            for (auto &kv : listing_) {
                if (tables_.find(kv.first) == tables_.end() && kv.first != key) {
                    batch.push_back(kv.second->name);
                }
            }
            std::unordered_map<string, SalesforceDescribe> got;
            if (session.ToolingDescribe(batch, got)) {
                for (auto &kv : got) {
                    if (tables_.find(kv.first) != tables_.end() || kv.second.fields.empty()) {
                        continue;
                    }
                    bool any_unknown = false;
                    for (auto &f : kv.second.fields) {
                        if (f.unknown_type) {
                            any_unknown = true;
                            break;
                        }
                    }
                    if (any_unknown) {
                        continue; // ambiguous type -> leave for REST fallback (lazy)
                    }
                    BuildAndCacheEntry(kv.second);
                }
                auto hit = tables_.find(key);
                if (hit != tables_.end()) {
                    return hit->second.get(); // requested object served from Tooling
                }
            }
            // Tooling failed / object absent / ambiguous type -> authoritative REST.
        }

        // REST describe (default + fallback): describe once and cache (#12).
        IncDescribeCalls(); // DEBUG/TEST counter — proves describe-once
        SalesforceDescribe describe = session.Describe(object);

        // Parent relationship expansion (#v0.6 §7), opt-in + describe-source only.
        string relationships = "off";
        Value rv;
        if (transaction.GetContext().TryGetCurrentSetting("sf_relationships", rv) && !rv.IsNull()) {
            relationships = StringUtil::Lower(rv.ToString());
        }
        // Read effective depth regardless of mode so the diagnostics config row
        // always reports it (#v1.0). 1 = parent only (default), 2 = + grandparent.
        int depth = 1;
        Value dv;
        if (transaction.GetContext().TryGetCurrentSetting("sf_relationship_depth", dv) &&
            !dv.IsNull()) {
            int64_t d = dv.GetValue<int64_t>();
            depth = d < 1 ? 1 : (d > 2 ? 2 : static_cast<int>(d)); // clamp 1..2
        }
        // Stamp the relationship-diagnostics snapshot on EVERY resolution — even
        // when expansion is off — so salesforce_relationships() always has a
        // config row (off must not look like an empty bug). Relationship rows
        // are recorded inside BuildRelationshipFields when mode == parent.
        RelDiagBegin(describe.object_name, relationships, depth);
        if (relationships == "parent") {
            ExpandParentRelationships(session, describe, depth);
        }
        return BuildAndCacheEntry(describe);
    }

    optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction, CreateFunctionInfo &) override { Unsupported("CREATE FUNCTION"); }
    optional_ptr<CatalogEntry> CreateTable(CatalogTransaction, BoundCreateTableInfo &) override { Unsupported("CREATE TABLE"); }
    optional_ptr<CatalogEntry> CreateView(CatalogTransaction, CreateViewInfo &) override { Unsupported("CREATE VIEW"); }
    optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction, CreateSequenceInfo &) override { Unsupported("CREATE SEQUENCE"); }
    optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) override { Unsupported("CREATE TABLE FUNCTION"); }
    optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) override { Unsupported("CREATE COPY FUNCTION"); }
    optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) override { Unsupported("CREATE PRAGMA FUNCTION"); }
    optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction, CreateCollationInfo &) override { Unsupported("CREATE COLLATION"); }
    optional_ptr<CatalogEntry> CreateType(CatalogTransaction, CreateTypeInfo &) override { Unsupported("CREATE TYPE"); }
    optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) override { Unsupported("CREATE INDEX"); }
    void DropEntry(ClientContext &, DropInfo &) override { Unsupported("DROP"); }
    void Alter(CatalogTransaction, AlterInfo &) override { Unsupported("ALTER"); }

private:
    // Build a table entry from a described schema and insert it into the cache.
    // Caller MUST hold lock_. Drops compound fields; sets NOT NULL constraints.
    SalesforceTableEntry *BuildAndCacheEntry(const SalesforceDescribe &describe) {
        CreateTableInfo info(catalog.GetName(), this->name, describe.object_name);
        vector<SalesforceField> queryable;
        for (auto &f : describe.fields) {
            if (!IsQueryableField(f)) {
                continue;
            }
            info.columns.AddColumn(ColumnDefinition(f.name, f.duckdb_type));
            if (!f.nillable) {
                info.constraints.push_back(
                    make_uniq<NotNullConstraint>(LogicalIndex(queryable.size())));
            }
            queryable.push_back(f);
        }
        auto entry = make_uniq<SalesforceTableEntry>(catalog, *this, info, config_, token_,
                                                     std::move(queryable));
        auto *ptr = entry.get();
        tables_.emplace(StringUtil::Lower(describe.object_name), std::move(entry));
        return ptr;
    }

    // Describe a parent object once, cached for this catalog (reuses the #12
    // describe budget). Caller holds lock_. Returns nullptr on failure.
    const SalesforceDescribe *GetParentDescribe(SalesforceSession &session, const string &object) {
        string key = StringUtil::Lower(object);
        auto it = parent_describe_cache_.find(key);
        if (it != parent_describe_cache_.end()) {
            return &it->second;
        }
        try {
            IncDescribeCalls();
            SalesforceDescribe pd = session.Describe(object);
            auto res = parent_describe_cache_.emplace(key, std::move(pd));
            return &res.first->second;
        } catch (...) {
            return nullptr; // un-describable parent -> relationship simply skipped
        }
    }

    // Append synthesised parent-relationship STRUCT columns (#v0.6 §7, depth-2
    // #v1.0). `depth` = how many parent levels to expand (1 = parent only,
    // 2 = + grandparent). Caller holds lock_.
    void ExpandParentRelationships(SalesforceSession &session, SalesforceDescribe &describe,
                                   int depth) {
        std::unordered_set<string> visited;
        visited.insert(StringUtil::Lower(describe.object_name));
        auto rels = BuildRelationshipFields(session, describe, depth, visited, 1);
        for (auto &e : rels) {
            describe.fields.push_back(std::move(e));
        }
    }

    // Build the relationship STRUCT fields for `desc`, expanding up to `depth`
    // parent levels. Single-target only (polymorphic skipped); self + already-
    // visited objects skipped (cycle guard). A nested level becomes a STRUCT
    // child of its parent STRUCT. Caller holds lock_.
    vector<SalesforceField> BuildRelationshipFields(SalesforceSession &session,
                                                    const SalesforceDescribe &desc, int depth,
                                                    std::unordered_set<string> &visited, int level) {
        vector<SalesforceField> rels;
        if (depth <= 0) {
            return rels;
        }
        for (auto &f : desc.fields) {
            if (StringUtil::Lower(f.sf_type) != "reference") {
                continue; // not a relationship candidate -> not recorded
            }
            // Each skip below records a diagnostic row (#v1.0) with its reason
            // and parent level, then continues — behaviour is unchanged.
            if (f.relationship_name.empty()) {
                RelDiagRecord(f.name, "", level, "skipped", "no_relationship_name", -1);
                continue;
            }
            if (f.reference_to.size() != 1) {
                RelDiagRecord(f.relationship_name, "", level, "skipped", "polymorphic", -1);
                continue; // 0 or >1 targets -> no single STRUCT target
            }
            const string &parent = f.reference_to[0];
            string plow = StringUtil::Lower(parent);
            if (StringUtil::CIEquals(parent, desc.object_name)) {
                RelDiagRecord(f.relationship_name, parent, level, "skipped", "self_reference", -1);
                continue;
            }
            if (visited.count(plow)) {
                RelDiagRecord(f.relationship_name, parent, level, "skipped", "cycle", -1);
                continue;
            }
            bool collide = false; // avoid colliding with an existing column name
            for (auto &g : desc.fields) {
                if (StringUtil::CIEquals(g.name, f.relationship_name)) {
                    collide = true;
                    break;
                }
            }
            if (collide) {
                RelDiagRecord(f.relationship_name, parent, level, "skipped", "name_collision", -1);
                continue;
            }
            const SalesforceDescribe *pd = GetParentDescribe(session, parent);
            if (!pd) {
                RelDiagRecord(f.relationship_name, parent, level, "skipped",
                              "parent_not_describable", -1);
                continue;
            }
            SalesforceField rel;
            rel.is_relationship = true;
            rel.name = f.relationship_name;
            rel.relationship_name = f.relationship_name;
            child_list_t<LogicalType> struct_children;
            for (auto &pf : pd->fields) {
                if (!IsQueryableField(pf)) {
                    continue; // drop compound
                }
                SalesforceField child = pf; // scalar copy
                child.is_relationship = false;
                child.children.clear();
                struct_children.emplace_back(child.name, child.duckdb_type);
                rel.children.push_back(std::move(child));
            }
            // Recurse one more parent level (grandparent) as nested STRUCT children.
            visited.insert(plow);
            auto nested = BuildRelationshipFields(session, *pd, depth - 1, visited, level + 1);
            visited.erase(plow);
            for (auto &n : nested) {
                bool dup = false; // don't shadow a parent scalar child of the same name
                for (auto &c : rel.children) {
                    if (StringUtil::CIEquals(c.name, n.name)) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                struct_children.emplace_back(n.name, n.duckdb_type);
                rel.children.push_back(std::move(n));
            }
            if (rel.children.empty()) {
                RelDiagRecord(f.relationship_name, parent, level, "skipped", "no_fields", -1);
                continue;
            }
            RelDiagRecord(f.relationship_name, parent, level, "expanded", "",
                          static_cast<int64_t>(rel.children.size()));
            rel.duckdb_type = LogicalType::STRUCT(std::move(struct_children));
            rels.push_back(std::move(rel));
        }
        return rels;
    }

    void EmitResolved(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
        if (type != CatalogType::TABLE_ENTRY) {
            return;
        }
        std::lock_guard<std::mutex> g(lock_);
        // Resolved objects: full-schema entries.
        for (auto &kv : tables_) {
            callback(*kv.second);
        }
        // Discovered-but-unreferenced objects: name-only (0-column) listing
        // entries, skipping any already resolved above (avoid duplicates).
        for (auto &kv : listing_) {
            if (tables_.find(kv.first) == tables_.end()) {
                callback(*kv.second);
            }
        }
    }

    // One-time global object discovery (GET /sobjects, queryable only) into the
    // 0-column listing_ entries. Triggered by Scan (SHOW TABLES / duckdb_tables /
    // information_schema.tables) — never at ATTACH. Field schema stays lazy.
    void EnsureObjectListLoaded(ClientContext &context) {
        std::lock_guard<std::mutex> g(lock_);
        if (object_list_loaded_) {
            return;
        }
        auto client = BuildHttpClientForContext(context);
        SalesforceSession session(config_, *client);
        session.SetToken(token_);
        IncGlobalDescribeCalls(); // DEBUG/TEST counter
        for (auto &obj : session.GlobalDescribe()) {
            string key = StringUtil::Lower(obj);
            if (tables_.find(key) != tables_.end() || listing_.find(key) != listing_.end()) {
                continue;
            }
            CreateTableInfo info(catalog.GetName(), this->name, obj);
            listing_.emplace(key, make_uniq<SalesforceTableEntry>(catalog, *this, info, config_,
                                                                  token_, vector<SalesforceField>{}));
        }
        object_list_loaded_ = true;
    }

    [[noreturn]] static void Unsupported(const char *op) {
        throw NotImplementedException(
            std::string("Salesforce ATTACH catalog is read-only — ") + op +
            " is not supported.");
    }

    SalesforceConfig config_;
    SalesforceTokenSet token_;
    std::mutex lock_;
    // Global object listing (#14): name-only (0-column) entries for enumeration.
    // Populated once by EnsureObjectListLoaded; in-memory, dropped at DETACH.
    bool object_list_loaded_ = false;
    std::unordered_map<string, unique_ptr<SalesforceTableEntry>> listing_;
    // Per-catalog in-memory metadata cache: lower(object) -> described schema
    // (a TableCatalogEntry). Populated lazily on first reference; an object is
    // described exactly once per ATTACH. Invalidated when the catalog is
    // destroyed at DETACH. In-memory only — never persisted (#12).
    std::unordered_map<string, unique_ptr<SalesforceTableEntry>> tables_;
    // Parent-object describes for relationship expansion (#v0.6 §7), cached per
    // catalog so a parent is described once regardless of how many child fields
    // (or child objects) reference it.
    std::unordered_map<string, SalesforceDescribe> parent_describe_cache_;
};

// ---------------------------------------------------------------------------
//  SalesforceCatalog
// ---------------------------------------------------------------------------

class SalesforceCatalog final : public Catalog {
public:
    SalesforceCatalog(AttachedDatabase &db, SalesforceConfig config, SalesforceTokenSet token)
        : Catalog(db), config_(std::move(config)), token_(std::move(token)) {}

    string GetCatalogType() override { return "salesforce"; }

    void Initialize(bool) override {
        ResetDescribeCalls();       // DEBUG/TEST: baseline per-catalog describe counter
        ResetGlobalDescribeCalls(); // DEBUG/TEST: baseline global describe counter
        ResetToolingCalls();        // DEBUG/TEST: baseline Tooling-query counter
        CreateSchemaInfo info;
        info.schema = SALESFORCE_MAIN_SCHEMA;
        info.on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
        main_schema_ = make_uniq<SalesforceSchemaEntry>(*this, info, config_, token_);
    }

    bool InMemory() override { return false; }
    string GetDBPath() override { return "salesforce://" + config_.org; }
    DatabaseSize GetDatabaseSize(ClientContext &) override { return DatabaseSize(); }

    void ScanSchemas(ClientContext &, std::function<void(SchemaCatalogEntry &)> callback) override {
        if (main_schema_) {
            callback(*main_schema_);
        }
    }

    optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction, const EntryLookupInfo &schema_lookup,
                                                  OnEntryNotFound if_not_found) override {
        const auto &name = schema_lookup.GetEntryName();
        if (name.empty() || name == SALESFORCE_MAIN_SCHEMA || name == DEFAULT_SCHEMA) {
            return main_schema_.get();
        }
        if (if_not_found == OnEntryNotFound::RETURN_NULL) {
            return nullptr;
        }
        throw BinderException("Schema '%s' not found in Salesforce catalog (only 'main' is exposed)", name);
    }

    optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction, CreateSchemaInfo &) override {
        throw NotImplementedException("CREATE SCHEMA is not supported on a Salesforce catalog (read-only).");
    }
    void DropSchema(ClientContext &, DropInfo &) override {
        throw NotImplementedException("DROP SCHEMA is not supported on a Salesforce catalog.");
    }

    PhysicalOperator &PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &, PhysicalOperator &) override {
        throw NotImplementedException("CTAS not supported on Salesforce catalog");
    }
    PhysicalOperator &PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &, optional_ptr<PhysicalOperator>) override {
        throw NotImplementedException("INSERT not supported on Salesforce catalog");
    }
    PhysicalOperator &PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &, PhysicalOperator &) override {
        throw NotImplementedException("DELETE not supported on Salesforce catalog");
    }
    PhysicalOperator &PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &, PhysicalOperator &) override {
        throw NotImplementedException("UPDATE not supported on Salesforce catalog");
    }
    PhysicalOperator &PlanMergeInto(ClientContext &, PhysicalPlanGenerator &, LogicalMergeInto &, PhysicalOperator &) override {
        throw NotImplementedException("MERGE not supported on Salesforce catalog");
    }
    unique_ptr<LogicalOperator> BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                unique_ptr<LogicalOperator>) override {
        throw NotImplementedException("CREATE INDEX not supported on Salesforce catalog");
    }
    unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &, TableCatalogEntry &, unique_ptr<LogicalOperator>,
                                                  unique_ptr<CreateIndexInfo>, unique_ptr<AlterTableInfo>) override {
        throw NotImplementedException("ALTER ... ADD INDEX not supported on Salesforce catalog");
    }

private:
    SalesforceConfig config_;
    SalesforceTokenSet token_;
    unique_ptr<SalesforceSchemaEntry> main_schema_;
};

// ---------------------------------------------------------------------------
//  SalesforceTransactionManager (no-op, read-only)
// ---------------------------------------------------------------------------

class SalesforceTransaction final : public Transaction {
public:
    SalesforceTransaction(TransactionManager &manager, ClientContext &context)
        : Transaction(manager, context) {}
};

class SalesforceTransactionManager final : public TransactionManager {
public:
    explicit SalesforceTransactionManager(AttachedDatabase &db) : TransactionManager(db) {}

    Transaction &StartTransaction(ClientContext &context) override {
        auto tx = make_uniq<SalesforceTransaction>(*this, context);
        auto &ref = *tx;
        std::lock_guard<std::mutex> g(lock_);
        transactions_.emplace(&ref, std::move(tx));
        return ref;
    }
    ErrorData CommitTransaction(ClientContext &, Transaction &transaction) override {
        std::lock_guard<std::mutex> g(lock_);
        transactions_.erase(&transaction);
        return ErrorData();
    }
    void RollbackTransaction(Transaction &transaction) override {
        std::lock_guard<std::mutex> g(lock_);
        transactions_.erase(&transaction);
    }
    void Checkpoint(ClientContext &, bool) override {}

private:
    std::mutex lock_;
    std::unordered_map<Transaction *, unique_ptr<SalesforceTransaction>> transactions_;
};

// ---------------------------------------------------------------------------
//  StorageExtension wiring
// ---------------------------------------------------------------------------

static unique_ptr<Catalog> SalesforceAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context,
                                            AttachedDatabase &db, const string &,
                                            AttachInfo &attach_info, AttachOptions &) {
    // #2 parse + validate; #3 OAuth exchange (token in memory only).
    SalesforceConfig config =
        SalesforceConfig::ParseAndValidate(attach_info.path, attach_info, context);
    auto http = BuildHttpClientForContext(context);
    SalesforceTokenSet token = SalesforceAuth::ExchangeRefreshToken(config, *http);
    return make_uniq_base<Catalog, SalesforceCatalog>(db, std::move(config), std::move(token));
}

static unique_ptr<TransactionManager>
SalesforceCreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db, Catalog &) {
    return make_uniq_base<TransactionManager, SalesforceTransactionManager>(db);
}

unique_ptr<StorageExtension> GetSalesforceStorageExtension() {
    auto ext = make_uniq<StorageExtension>();
    ext->attach = SalesforceAttach;
    ext->create_transaction_manager = SalesforceCreateTransactionManager;
    return ext;
}

} // namespace duckdb

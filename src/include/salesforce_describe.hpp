#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// One column of a described sObject.
struct SalesforceField {
    string name;
    string sf_type;
    LogicalType duckdb_type = LogicalType::VARCHAR;
    bool nillable = true;
    int64_t length = 0;
    int64_t precision = 0;
    int64_t scale = 0;
    bool filterable = false;
    bool sortable = false;
    bool unknown_type = false; // sf_type not recognised -> mapped to VARCHAR

    // Parent relationship (#v0.6 §7). For a `reference` field, describe carries
    // the relationship name + target object(s). When relationship expansion is
    // ON, an extra STRUCT column is synthesised (is_relationship=true) named by
    // relationship_name, whose `children` are the parent's scalar fields; its
    // duckdb_type is STRUCT(children...). Depth 1 only; polymorphic skipped.
    string relationship_name;     // describe "relationshipName"
    vector<string> reference_to;  // describe "referenceTo" (target sObjects)
    vector<string> picklist_values; // describe "picklistValues[*].value" (empty if none)
    bool is_relationship = false; // true => synthesised parent STRUCT column
    // NOTE: child relationships live on SalesforceDescribe (below), not here.
    vector<SalesforceField> children; // parent scalar fields (for is_relationship)
};

// One child (one-to-many) relationship from describe "childRelationships"
// (#v1.6 §18 cut 2). `relationship_name` may be empty (null in describe) — such
// a child is not SOQL-subquery-addressable by name.
struct SalesforceChildRelationship {
    string child_object;      // "childSObject"
    string field;             // "field" — the child's FK back to this object
    string relationship_name; // "relationshipName" (may be empty)
};

struct SalesforceDescribe {
    string object_name;
    vector<SalesforceField> fields;
    vector<SalesforceChildRelationship> child_relationships; // describe "childRelationships"
};

// Parse an sObject describe JSON response into a SalesforceDescribe. The
// fallback_object name is used if the response has no top-level "name".
SalesforceDescribe ParseDescribe(const string &json, const string &fallback_object);

// salesforce_describe(object, client_id:=, client_secret:=, refresh_token:=,
//   login_url:=, api_version:=) -> one row per field of the sObject's schema.
TableFunction GetSalesforceDescribeFunction();

} // namespace duckdb

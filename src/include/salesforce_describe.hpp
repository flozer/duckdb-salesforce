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
};

struct SalesforceDescribe {
    string object_name;
    vector<SalesforceField> fields;
};

// Parse an sObject describe JSON response into a SalesforceDescribe. The
// fallback_object name is used if the response has no top-level "name".
SalesforceDescribe ParseDescribe(const string &json, const string &fallback_object);

// salesforce_describe(object, client_id:=, client_secret:=, refresh_token:=,
//   login_url:=, api_version:=) -> one row per field of the sObject's schema.
TableFunction GetSalesforceDescribeFunction();

} // namespace duckdb

#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

// salesforce_query(soql, client_id:=, client_secret:=, refresh_token:=,
//   login_url:=, api_version:=) -> one row per fetched record:
//   (record_index BIGINT, record_json VARCHAR).
//
// Issue #6: a paginated fetcher surface. Records are returned as raw JSON; row
// -> typed DuckDB vector decoding is issue #7.
TableFunction GetSalesforceQueryFunction();

// sf_url_encode(VARCHAR) -> VARCHAR. Percent-encodes a URL component; exposed
// so the SOQL q= encoding can be tested directly.
ScalarFunction GetSalesforceUrlEncodeFunction();

} // namespace duckdb

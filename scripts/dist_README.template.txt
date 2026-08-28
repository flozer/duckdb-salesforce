duckdb-salesforce @@VERSION@@ - @@PLATFORM@@

Files in this archive:
  salesforce.duckdb_extension  - the DuckDB extension binary

Requirements:
  1. DuckDB CLI v1.5.3, v1.5.4, or v1.5.5, or another ABI-compatible v1.5.x build
  2. Salesforce Connected App credentials or JWT bearer setup
  3. TLS certificate verification remains enabled

Quick start for a local unsigned artifact:
  duckdb -unsigned
  SET allow_unsigned_extensions = true;
  LOAD '/path/to/salesforce.duckdb_extension';

  -- Prefer environment variables for credentials:
  --   SF_CLIENT_ID, SF_CLIENT_SECRET, SF_REFRESH_TOKEN, optional SF_LOGIN_URL
  ATTACH 'salesforce://myorg' AS sf (TYPE salesforce, auth_source 'env');
  SELECT Id, Name FROM sf.Account LIMIT 10;

The -unsigned flag is required because this local archive is not signed by the
DuckDB extension authority. Once duckdb-salesforce is published through
community-extensions, use:

  INSTALL salesforce FROM community;
  LOAD salesforce;

Documentation:
  https://github.com/flozer/duckdb-salesforce/tree/main/docs

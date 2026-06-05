-- §11 premise check (MANUAL, real org). Question: does Bulk API 2.0 query CSV
-- actually return a base64/blob field, or not?
--
-- Run with the statically-linked Release shell + env auth (the v0.9 runner
-- pattern):  Get-Content -Raw scripts/smoke_base64_bulk.sql | build/release/duckdb.exe -batch
-- (export SF_CLIENT_ID / SF_CLIENT_SECRET / SF_REFRESH_TOKEN first.)
--
-- base64 Salesforce fields map to DuckDB BLOB. Output is metadata + at most one
-- row; no secret is selected.

ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env');

.print === 1. DISCOVER base64/BLOB columns on common blob-bearing objects ===
-- Pick whichever object exists in your org AND has at least one row you can read.
-- (DESCRIBE resolves the schema without fetching data.)
.print -- Attachment:
SELECT column_name, column_type
FROM (DESCRIBE SELECT * FROM sf.Attachment) WHERE column_type = 'BLOB';
.print -- ContentVersion (VersionData):
SELECT column_name, column_type
FROM (DESCRIBE SELECT * FROM sf.ContentVersion) WHERE column_type = 'BLOB';
-- If neither exists, find any BLOB column on a readable object and edit step 2/3.

.print === 2. REST baseline: does REST return the blob? (expected: yes) ===
SET sf_force_transport = 'rest';
SELECT Id, octet_length(Body) AS body_bytes
FROM sf.Attachment
WHERE Body != ''::BLOB
LIMIT 1;
SELECT transport, reason FROM salesforce_last_transport();

.print === 3. DECISIVE: force BULK and project the blob field ===
-- Interpretation:
--   * Salesforce ERROR about blob/base64/unsupported  -> premise TRUE  (implement guard, migrate tests)
--   * returns non-NULL body_bytes                      -> premise FALSE (drop guard, defer §11)
--   * returns the row but body_bytes is NULL/0 only    -> Bulk omits the blob -> premise effectively TRUE
--   * no rows / no permission                          -> inconclusive; try ContentVersion.VersionData or another object
SET sf_force_transport = 'bulk';
SELECT Id, octet_length(Body) AS body_bytes
FROM sf.Attachment
LIMIT 1;
SELECT transport, reason FROM salesforce_last_transport();

SET sf_force_transport = 'rest';
DETACH sf;
.print === base64-bulk premise check complete ===

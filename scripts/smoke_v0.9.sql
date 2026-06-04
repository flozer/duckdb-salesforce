-- LIVE smoke for v0.9.0 — runs against a REAL Salesforce org.
-- NOT an offline test. Requires credentials in the environment (see
-- scripts/run_smoke_v0.9.ps1). The statically-linked build/release/duckdb shell
-- already has the extension built in (no LOAD needed).
--
-- Output is ORG data (counts, industries, schema, SOQL) — review before
-- sharing. No token/secret is ever selected or printed.

-- Auth from environment (SF_CLIENT_ID/SECRET/REFRESH_TOKEN, or swap to sfdx_url).
ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env');

.print === 1. basic SELECT + COUNT(*) pushdown ===
SELECT count(*) AS account_rows FROM sf.Account;

.print === 2. queryAll (archived + soft-deleted) ===
SET sf_query_mode = 'queryAll';
SELECT count(*) AS account_rows_queryall FROM sf.Account;
SET sf_query_mode = 'query';

.print === 3. salesforce_aggregate() simple ===
SELECT * FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n, MIN(CreatedDate) min_created, MAX(CreatedDate) max_created');

.print === 4. salesforce_aggregate() GROUP BY ===
SELECT * FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry') ORDER BY CAST(n AS BIGINT) DESC LIMIT 5;

.print === 5. grandparent traversal (schema only, no data) ===
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;
SELECT column_name, column_type FROM (DESCRIBE SELECT * FROM sf.Contact) WHERE column_name = 'Account';

.print === 6. salesforce_relationships() diagnostics ===
SELECT row_type, relationships_mode, relationship_depth, expanded_count, skipped_count
FROM salesforce_relationships() WHERE row_type = 'config';
SELECT relationship_name, parent_object, depth_level, status, reason
FROM salesforce_relationships() WHERE row_type = 'relationship' LIMIT 10;
SET sf_relationships = 'off';
SET sf_relationship_depth = 1;

.print === 7. simple Bulk scan ===
SET sf_force_transport = 'bulk';
SELECT count(*) AS account_rows_bulk FROM sf.Account;
SET sf_force_transport = 'rest';

.print === 8. last SOQL (secret-free diagnostic) ===
SELECT soql FROM salesforce_last_soql();

DETACH sf;
.print === smoke complete ===

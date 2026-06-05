-- Light smoke for v0.9.1 — runs against a REAL org. Focus: the new
-- metadata-driven functions + a sanity check that normal scans still work.
-- NOT an offline test. Requires env credentials (see run_smoke_v0.9.1.ps1).
-- The shell build/release/duckdb has the extension built in (no LOAD needed).
--
-- Output is org CONFIGURATION metadata (picklist labels, record type names) +
-- row counts — review before sharing. No token/secret is selected.
--
-- Edit the object/field below if your org uses different ones (Account.Industry
-- is a common standard picklist; pick an object that actually has record types).

ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env');

.print === 1. picklist values (full catalog: active + inactive) ===
SELECT value, label, active, is_default
FROM salesforce_picklist_values('sf', 'Account', 'Industry')
ORDER BY active DESC, value
LIMIT 25;

.print === 1b. active-only picklist values ===
SELECT count(*) AS active_values
FROM salesforce_picklist_values('sf', 'Account', 'Industry') WHERE active;

.print === 2. record types ===
SELECT developer_name, label, active, is_default
FROM salesforce_record_types('sf', 'Account')
ORDER BY developer_name;

.print === 3. refresh metadata (object then global) ===
SELECT * FROM salesforce_refresh_metadata('sf', 'Account');
SELECT * FROM salesforce_refresh_metadata('sf');

.print === 4. normal REST scan still works ===
SET sf_force_transport = 'rest';
SELECT count(*) AS account_rows_rest FROM sf.Account;

.print === 5. normal Bulk scan (non-blob) still works ===
SET sf_force_transport = 'bulk';
SELECT count(*) AS account_rows_bulk FROM sf.Account;
SET sf_force_transport = 'rest';

.print === 6. picklist re-reads after refresh (cache repopulated) ===
SELECT count(*) AS industry_values
FROM salesforce_picklist_values('sf', 'Account', 'Industry');

DETACH sf;
.print === v0.9.1 light smoke complete ===

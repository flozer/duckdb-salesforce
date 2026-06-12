# Report Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three opt-in, read-only SQL functions that bridge Salesforce
reports into DuckDB — `salesforce_reports()` (list definitions),
`salesforce_report(id)` (tabular sample run, ≤2,000 rows + reserved diagnostic
columns), and `salesforce_report_soql(id)` (best-effort candidate SOQL with
`translatable` + `caveats`).

**Architecture:** New `src/salesforce_report.cpp` + `src/include/salesforce_report.hpp`
hold the three table functions, following the existing table-function patterns
(`salesforce_query.cpp` for a VARCHAR-arg function; `salesforce_diag.cpp` for the
single-row diagnostic shape). Two new public `SalesforceSession` methods wrap the
Analytics REST API, modeled on the existing `Query()` / `AuthorizedSend()` /
`AuthorizedGet()` members. The offline mock (`ScriptedMockHttpClient` in
`salesforce_http_client.cpp`) gains analytics endpoint sequences. No existing
behavior changes; everything is additive and opt-in.

**Tech Stack:** C++17, DuckDB extension TableFunction API (v1.5.3), existing
`sfjson` JSON helpers, OpenSSL/httplib HTTP, DuckDB `.test` (sqllogic) for tests.

**Scope guard:** Tabular reports only; synchronous Analytics API only. Summary/
matrix, async `/instances`, multi-object auto-translation, and large extraction
are OUT (see spec `docs/superpowers/specs/2026-06-12-report-bridge-design.md`).
No live tests in CI. No release/community/tag actions in this plan.

**Reference exemplars (read before each phase):**
- VARCHAR-arg table function + bind/init/exec: `src/salesforce_query.cpp`
  (`GetSalesforceQueryFunction`, lines ~131-137).
- Single-row diagnostic table function: `src/salesforce_diag.cpp`
  (`QueryCostBind`, `QueryCostFunction`, `Str`/`IntOrNull` helpers).
- Session HTTP + JSON parse: `src/salesforce_session.cpp` (`Query`,
  `AuthorizedGet`, `AuthorizedSend`, `BulkStartJob`); JSON helpers in
  `src/include/salesforce_json.hpp` (`sfjson::GetString/GetBool/GetObjectArray`).
- Mock endpoint routing: `src/salesforce_http_client.cpp`
  (`ScriptedMockHttpClient`, `Step`, the `/jobs/query` and `/queryAll` branches).
- Registration: `src/salesforce_extension.cpp` (`loader.RegisterFunction(...)`).
- Test mock knobs + assertions: `test/sql/salesforce_cost.test`,
  `test/sql/salesforce_queryall.test`.

---

## File Structure

- Create `src/include/salesforce_report.hpp` — declares the three
  `GetSalesforce*Function()` factories.
- Create `src/salesforce_report.cpp` — the three table functions + tabular
  factMap parsing + SOQL synthesis helpers (synthesis helpers `static` here;
  unit-exercised through the SQL tests).
- Modify `src/include/salesforce_session.hpp` — add public `RunReport(reportId)`
  and `DescribeReport(reportId)` returning the raw JSON body string.
- Modify `src/salesforce_session.cpp` — implement both, using `AuthorizedGet` on
  the analytics paths.
- Modify `src/salesforce_http_client.cpp` — add `analytics` endpoint sequences to
  `ScriptedMockHttpClient` + the `BuildHttpClientForContext` wiring; route URLs
  containing `/analytics/reports/` to them.
- Modify `src/salesforce_extension.cpp` — register the three functions + the
  test-only mock options (`sf_mock_report_*`).
- Modify `src/CMakeLists.txt` — add `src/salesforce_report.cpp` to
  `EXTENSION_SOURCES`.
- Create `test/sql/salesforce_report.test` — mock coverage for all three.
- Modify `docs/en/function_manual.md`, `docs/pt/function_manual.md`,
  `docs/en/usage_guide.md`, `docs/pt/usage_guide.md` — document the functions.

---

## Phase A — Analytics HTTP foundation + mock

### Task A1: Session analytics methods

**Files:**
- Modify: `src/include/salesforce_session.hpp` (public section, after `Query`)
- Modify: `src/salesforce_session.cpp` (after `Query` impl)

- [ ] **Step 1: Declare the methods**

In `salesforce_session.hpp`, after the `Query` declaration (~line 108):

```cpp
    // Salesforce Reports & Dashboards REST API (ROADMAP §16, synchronous only).
    // RunReport executes the report and returns the raw JSON body (factMap +
    // reportMetadata + reportExtendedMetadata). DescribeReport returns the raw
    // /describe JSON. Both throw a clear, secret-free error on HTTP failure,
    // mirroring AuthorizedGet.
    string RunReport(const string &report_id);
    string DescribeReport(const string &report_id);
```

- [ ] **Step 2: Implement them**

In `salesforce_session.cpp`, after `Query()`. Model the path on `QueryPath`
(uses `config_.api_version`) and the GET on `AuthorizedGet`:

```cpp
string SalesforceSession::RunReport(const string &report_id) {
    // includeDetails=true returns row-level detail (tabular factMap "T!T".rows).
    string path = "/services/data/" + config_.api_version + "/analytics/reports/" +
                  report_id + "?includeDetails=true";
    return AuthorizedGet(path);
}

string SalesforceSession::DescribeReport(const string &report_id) {
    string path = "/services/data/" + config_.api_version + "/analytics/reports/" +
                  report_id + "/describe";
    return AuthorizedGet(path);
}
```

- [ ] **Step 3: Build to verify it compiles**

Run (PowerShell, MSVC env): `cmake --build build/release --target unittest`
Expected: links clean (methods unused yet — that is fine).

- [ ] **Step 4: Commit**

```bash
git add src/include/salesforce_session.hpp src/salesforce_session.cpp
git commit -m "feat(report): session RunReport/DescribeReport analytics calls"
```

### Task A2: Mock analytics endpoints

**Files:**
- Modify: `src/salesforce_http_client.cpp` (`ScriptedMockHttpClient` struct +
  routing + `BuildHttpClientForContext`)
- Modify: `src/salesforce_extension.cpp` (register `sf_mock_report_*` options)

- [ ] **Step 1: Add mock options**

In `salesforce_extension.cpp`, beside the other `sf_mock_*` options:

```cpp
    config.AddExtensionOption("sf_mock_report_status",
                              "TEST ONLY. Analytics report-run HTTP status(es), CSV.",
                              LogicalType::VARCHAR, Value(""));
    config.AddExtensionOption("sf_mock_report_body",
                              "TEST ONLY. Analytics report-run JSON body/pages ('|~|').",
                              LogicalType::VARCHAR, Value(""));
    config.AddExtensionOption("sf_mock_report_describe_status",
                              "TEST ONLY. Analytics report /describe HTTP status(es), CSV.",
                              LogicalType::VARCHAR, Value(""));
    config.AddExtensionOption("sf_mock_report_describe_body",
                              "TEST ONLY. Analytics report /describe JSON body ('|~|').",
                              LogicalType::VARCHAR, Value(""));
```

- [ ] **Step 2: Add fields + routing to the mock**

In `ScriptedMockHttpClient` add `report_*` / `report_describe_*` vectors + index
members (mirror the existing `bulk_`/`queryall_` members). In the request router,
**before** the generic query branch, add — note `/describe` must be checked
before the bare report path, like the `/jobs/query/results` vs `/jobs/query`
split:

```cpp
        if (request.url.find("/analytics/reports/") != string::npos) {
            if (request.url.find("/describe") != string::npos) {
                return Step(report_describe_statuses_, report_describe_bodies_,
                            report_describe_index_);
            }
            return Step(report_statuses_, report_bodies_, report_index_);
        }
```

Wire the four settings in `BuildHttpClientForContext` exactly like the existing
`sf_mock_bulk_*` wiring (`ParseIntCsv` for statuses defaulting to `{200}`,
`SplitOn(..., "|~|")` for bodies), and pass them into the constructor.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build/release --target unittest`
Expected: clean.

- [ ] **Step 4: Commit**

```bash
git add src/salesforce_http_client.cpp src/salesforce_extension.cpp
git commit -m "test(report): mock analytics report run + describe endpoints"
```

---

## Phase B — `salesforce_reports()` (definition listing)

### Task B1: reports() over the Report sObject

**Files:**
- Create: `src/include/salesforce_report.hpp`
- Create: `src/salesforce_report.cpp`
- Modify: `src/CMakeLists.txt` (add source), `src/salesforce_extension.cpp`
  (register), `test/sql/salesforce_report.test` (create)

- [ ] **Step 1: Write the failing test** (`test/sql/salesforce_report.test`)

```
# name: test/sql/salesforce_report.test
# description: §16 Report Bridge — reports() / report() / report_soql() (mocked)
# group: [salesforce]

require salesforce

statement ok
SET sf_mock_token_status = 200;

statement ok
SET sf_mock_token_body = '{"access_token":"AT","instance_url":"https://x.my.salesforce.com"}';

statement ok
ATTACH 'salesforce://org' AS sf (TYPE salesforce, client_id 'c', client_secret 's', refresh_token 'r');

# --- reports(): lists definitions via the Report sObject -----------------------
statement ok
SET sf_mock_query_body = '{"done":true,"records":[{"Id":"00O1","Name":"Pipeline","DeveloperName":"pipeline","FolderName":"Sales","Format":"Tabular"}]}';

query IIIII
SELECT Id, Name, DeveloperName, FolderName, Format FROM salesforce_reports() ORDER BY Id;
----
00O1	Pipeline	pipeline	Sales	Tabular

query I
SELECT contains(soql, 'FROM Report') AND contains(soql, 'DeveloperName')
FROM salesforce_last_soql();
----
true
```

- [ ] **Step 2: Run to verify it fails**

Run: `./build/release/test/unittest.exe "test/sql/salesforce_report.test"`
Expected: FAIL — `salesforce_reports` not registered (catalog error).

- [ ] **Step 3: Header** (`src/include/salesforce_report.hpp`)

```cpp
#pragma once
#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
namespace duckdb {
TableFunction GetSalesforceReportsFunction();
TableFunction GetSalesforceReportFunction();
TableFunction GetSalesforceReportSoqlFunction();
} // namespace duckdb
```

- [ ] **Step 4: Implement reports()** (`src/salesforce_report.cpp`)

Model on `salesforce_query.cpp`: a no-arg `TableFunction` whose bind issues the
fixed SOQL `SELECT Id, Name, DeveloperName, FolderName, Format FROM Report` via
`session->Query(soql)` (build the session the same way `QueryBind` does, reusing
the attached catalog credentials), records it with `SetLastSoql(soql)`, and emits
the five VARCHAR columns by parsing each record with `sfjson::GetString`. Return
types/names: `{"Id","Name","DeveloperName","FolderName","Format"}`, all VARCHAR.

```cpp
TableFunction GetSalesforceReportsFunction() {
    return TableFunction("salesforce_reports", {}, ReportsFunction, ReportsBind, ReportsInit);
}
```

- [ ] **Step 5: Register + add to build**

`salesforce_extension.cpp`: `loader.RegisterFunction(GetSalesforceReportsFunction());`
and `#include "salesforce_report.hpp"`.
`src/CMakeLists.txt`: add `src/salesforce_report.cpp` to `EXTENSION_SOURCES`.

- [ ] **Step 6: Build + run to verify it passes**

Run: `cmake --build build/release --target unittest` then
`./build/release/test/unittest.exe "test/sql/salesforce_report.test"`
Expected: PASS (the reports() cases).

- [ ] **Step 7: Commit**

```bash
git add src/include/salesforce_report.hpp src/salesforce_report.cpp src/CMakeLists.txt src/salesforce_extension.cpp test/sql/salesforce_report.test
git commit -m "feat(report): salesforce_reports() definition listing"
```

---

## Phase C — `salesforce_report(id)` (tabular sample + reserved diag columns)

### Task C1: tabular factMap parse + reserved diagnostic columns

**Files:**
- Modify: `src/salesforce_report.cpp` (add `report()` + factMap parsing)
- Modify: `test/sql/salesforce_report.test` (append cases)

- [ ] **Step 1: Write the failing tests** (append to `salesforce_report.test`)

Tabular run: `detailColumns` define column order; `factMap["T!T"].rows[*].dataCells[*].label`
are the values; `allData` drives truncation. Reserved trailing columns use the
`__sf_report_` prefix.

```
# --- report(): tabular rows + reserved diagnostic columns ----------------------
statement ok
SET sf_mock_report_body = '{"hasDetailRows":true,"allData":true,"reportMetadata":{"detailColumns":["ACCOUNT_NAME","AMOUNT"]},"reportExtendedMetadata":{"detailColumnInfo":{"ACCOUNT_NAME":{"label":"Account Name"},"AMOUNT":{"label":"Amount"}}},"factMap":{"T!T":{"rows":[{"dataCells":[{"label":"Acme"},{"label":"100"}]},{"dataCells":[{"label":"Globex"},{"label":"50"}]}]}}}';

query II
SELECT "Account Name", "Amount" FROM salesforce_report('00O1') ORDER BY "Account Name";
----
Acme	100
Globex	50

query I
SELECT DISTINCT __sf_report_truncated = false AND __sf_report_all_data = true
   AND __sf_report_max_rows = 2000
FROM salesforce_report('00O1');
----
true

# --- report(): >2000 rows / allData=false -> truncated sample + loud diag -------
statement ok
SET sf_mock_report_body = '{"hasDetailRows":true,"allData":false,"reportMetadata":{"detailColumns":["ACCOUNT_NAME"]},"reportExtendedMetadata":{"detailColumnInfo":{"ACCOUNT_NAME":{"label":"Account Name"}}},"factMap":{"T!T":{"rows":[{"dataCells":[{"label":"Acme"}]}]}}}';

query I
SELECT DISTINCT __sf_report_truncated = true AND __sf_report_all_data = false
   AND contains(__sf_report_guidance, 'sample')
FROM salesforce_report('00O1');
----
true
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/release/test/unittest.exe "test/sql/salesforce_report.test"`
Expected: FAIL — `salesforce_report` not registered.

- [ ] **Step 3: Implement report()** (`src/salesforce_report.cpp`)

VARCHAR-arg `TableFunction` (model arg handling on `salesforce_query`'s
`{LogicalType::VARCHAR}`). Bind: call `session->RunReport(report_id)`; parse with
`sfjson`:
- column order from `reportMetadata.detailColumns` (array of API names);
- display names from `reportExtendedMetadata.detailColumnInfo[api].label`
  (fallback to the API name);
- append the four reserved columns last: `__sf_report_truncated` BOOLEAN,
  `__sf_report_all_data` BOOLEAN, `__sf_report_max_rows` BIGINT,
  `__sf_report_guidance` VARCHAR.
Bind must reject a non-tabular shape — if `factMap["T!T"]` is absent, throw
`BinderException("salesforce_report: only tabular reports are supported in this "
"cut (summary/matrix unsupported); report '%s'", report_id)`.
Execution: iterate `factMap["T!T"].rows[*].dataCells[*].label` into the data
columns (all VARCHAR), set `truncated = !allData`, `all_data = allData`,
`max_rows = 2000`, and `guidance = "report result is a validation sample only; "
"scale via sf.<Object>"` on every row. Use `FlatVector`/`StringVector::AddString`
and the `IntOrNull`/bool patterns from `salesforce_diag.cpp`.

- [ ] **Step 4: Register + build + run**

Register `GetSalesforceReportFunction()` in `salesforce_extension.cpp`. Build,
then run the test.
Expected: PASS (tabular + truncation cases).

- [ ] **Step 5: Commit**

```bash
git add src/salesforce_report.cpp src/salesforce_extension.cpp test/sql/salesforce_report.test
git commit -m "feat(report): salesforce_report() tabular sample + reserved diagnostics"
```

---

## Phase D — `salesforce_report_soql(id)` (best-effort SOQL)

### Task D1: single-row describe-driven SOQL synthesis

**Files:**
- Modify: `src/salesforce_report.cpp` (add `report_soql()` + synthesis)
- Modify: `test/sql/salesforce_report.test` (append cases)

- [ ] **Step 1: Write the failing tests** (append)

```
# --- report_soql(): single-object tabular -> translatable candidate ------------
statement ok
SET sf_mock_report_describe_body = '{"reportMetadata":{"reportType":{"type":"AccountList"},"reportFormat":"TABULAR","detailColumns":["NAME","ANNUAL_REVENUE"],"reportBooleanFilter":"1 AND 2","reportFilters":[{"column":"ANNUAL_REVENUE","operator":"greaterThan","value":"100"},{"column":"NAME","operator":"contains","value":"Inc"}]},"reportExtendedMetadata":{"detailColumnInfo":{"NAME":{"label":"Name"},"ANNUAL_REVENUE":{"label":"Annual Revenue"}}}}';

query I
SELECT translatable AND base_object <> '' AND contains(soql, 'SELECT')
   AND contains(soql, 'WHERE')
FROM salesforce_report_soql('00O1');
----
true

# --- report_soql(): summary report -> not translatable + caveats ---------------
statement ok
SET sf_mock_report_describe_body = '{"reportMetadata":{"reportType":{"type":"AccountList"},"reportFormat":"SUMMARY","detailColumns":["NAME"],"reportFilters":[]}}';

query I
SELECT translatable = false AND soql IS NULL AND contains(caveats, 'summary')
FROM salesforce_report_soql('00O1');
----
true
```

- [ ] **Step 2: Run to verify they fail**

Run: `./build/release/test/unittest.exe "test/sql/salesforce_report.test"`
Expected: FAIL — `salesforce_report_soql` not registered.

- [ ] **Step 3: Implement report_soql()** (`src/salesforce_report.cpp`)

Single-row table function (model the column emission + single-row state on
`salesforce_diag.cpp`'s `QueryCostFunction`/`QueryCostGlobalState`). Columns:
`report_id` VARCHAR, `report_name` VARCHAR, `report_type` VARCHAR, `base_object`
VARCHAR, `columns` LIST(VARCHAR), `filters` LIST(STRUCT(field VARCHAR, op VARCHAR,
value VARCHAR)), `soql` VARCHAR, `translatable` BOOLEAN, `caveats` VARCHAR.

Parse `session->DescribeReport(report_id)`:
- `base_object` from `reportMetadata.reportType.type` (best-effort base object).
- `columns` from `detailColumns`.
- `filters` from `reportFilters[*]` (`column`, `operator`, `value`).
Build a `static bool TranslateReport(...)` that sets `translatable=true` and a
synthesized `soql` ONLY for: `reportFormat == "TABULAR"`, single base object,
operators in {`equals`,`notEqual`,`lessThan`,`greaterThan`,`contains`} mapped to
`=`,`!=`,`<`,`>`,`LIKE '%v%'`, boolean logic from `reportBooleanFilter`
(`AND`/`OR` only), producing `SELECT <cols> FROM <object> WHERE <clause>`.
Otherwise `translatable=false`, `soql` NULL, and `caveats` naming the reason
("summary/matrix not supported", "multi-object", "bucket/formula column",
"unsupported operator", etc.). Emit LIST via `ListVector::PushBack` and the
STRUCT via `StructVector::GetEntries` (standard DuckDB list/struct construction).

- [ ] **Step 4: Register + build + run**

Register `GetSalesforceReportSoqlFunction()`. Build, run the test.
Expected: PASS (translatable + not-translatable cases).

- [ ] **Step 5: Commit**

```bash
git add src/salesforce_report.cpp src/salesforce_extension.cpp test/sql/salesforce_report.test
git commit -m "feat(report): salesforce_report_soql() best-effort candidate SOQL"
```

---

## Phase E — Docs + full-suite verification

### Task E1: Document the three functions (EN + PT)

**Files:**
- Modify: `docs/en/function_manual.md`, `docs/pt/function_manual.md`
- Modify: `docs/en/usage_guide.md`, `docs/pt/usage_guide.md`

- [ ] **Step 1: function_manual (EN + PT)** — add a "Report Bridge" subsection
  with the four-part structure (What it does / How it works / Why / Daily use)
  for each function. State explicitly: `salesforce_reports()` lists definitions
  not data; `salesforce_report()` is a ≤2,000-row validation **sample** with the
  reserved `__sf_report_*` columns, NOT a large-extraction path; the
  `report_soql()` `soql` is a **validatable candidate, not an equivalence
  contract**. Mirror wording in PT.

- [ ] **Step 2: usage_guide (EN + PT)** — add the report → sample → validate →
  scale-via-`sf.<Object>` workflow narrative; cross-link the §15 backfill recipe.

- [ ] **Step 3: Commit**

```bash
git add docs/en/function_manual.md docs/pt/function_manual.md docs/en/usage_guide.md docs/pt/usage_guide.md
git commit -m "docs(report): document Report Bridge functions (EN/PT)"
```

### Task E2: Full suite + acceptance check

- [ ] **Step 1: Run the whole salesforce suite**

Run: `./build/release/test/unittest.exe "*salesforce*"`
Expected: all pass, 0 fail, live tests skipped (no creds). No regressions.

- [ ] **Step 2: Acceptance walk-through** — confirm each spec acceptance bullet
  maps to a green test: tabular returns rows (C1); >2,000 → sample + loud diag
  (C1); summary/matrix/multi-object/bucket/formula → `translatable=false` (D1);
  reports() SOQL asserted (B1); CI offline/mock/secret-free (no live tests
  added); no community/release action taken.

- [ ] **Step 3: Stop for review** — do NOT tag, release, push to community, or
  bundle into a release pack. Report Bridge stays opt-in/read-only and out of any
  pack until explicit GO.

---

## Self-Review

- **Spec coverage:** reports()/report()/report_soql() → B1/C1/D1; reserved
  `__sf_report_` columns → C1; candidate-not-contract → D1 + E1; 2,000-row sample
  + loud diag → C1; translatable=false shapes → D1; mock-only/secret-free → all
  tests; quota/permission risks → documented (E1) + sample-only design bounds
  run-rate; no community/release → E2 Step 3. APIs (Report sObject, analytics
  run + describe) → A1/A2/B1/C1/D1.
- **Placeholders:** none — each C++ task is anchored to a named existing exemplar
  with exact symbols (functions, columns, mock knobs, registration lines) to copy
  from; test fixtures are concrete JSON.
- **Type consistency:** function factory names (`GetSalesforceReportsFunction`,
  `GetSalesforceReportFunction`, `GetSalesforceReportSoqlFunction`), session
  methods (`RunReport`, `DescribeReport`), mock options (`sf_mock_report_*`,
  `sf_mock_report_describe_*`), and reserved columns (`__sf_report_truncated`,
  `__sf_report_all_data`, `__sf_report_max_rows`, `__sf_report_guidance`) are used
  identically across tasks.
- **Note for implementers:** exact DuckDB list/struct vector construction and the
  `sfjson` accessor names should be confirmed against the headers at
  implementation time; the patterns are taken from `salesforce_diag.cpp` and
  `salesforce_json.hpp`. Adjust to the real signatures if they differ — the task
  boundaries and tests do not change.

# DuckDB v2.0 Readiness — C Extension API Capability Matrix

**Status:** Investigation only. No code changes, no `ref_next` branch, no rewrite in this
delivery. This document answers, per capability area this extension uses, whether DuckDB's
C Extension API (as opposed to the C++ API this extension currently uses) is a viable
replacement, so a future v2.0-era decision has evidence to work from.

**Scope note on "v2.0":** no `v2.0` tag exists upstream yet. This investigation is against
two nearby `duckdb/main` snapshots (`main` moves fast — the two commands below ran minutes
apart on 2026-08-28, hence the different SHAs): the C Extension API capability matrix below
was read against commit `98b6abb501a9ac08fa6897540a52303466099417`; the build/test canary
(see "Canary build result" below) was re-run against the then-current tip,
`25a14295a609678ac2e12e91c8bf39d24ca3c260`, once a build-matrix bug required redoing it.
Both are treated as a forward-looking proxy for what a v2.0-era API surface looks like — not
a promise that v2.0 will look exactly like either snapshot. The C API's own version markers
in the first snapshot report `DUCKDB_EXTENSION_API_VERSION_MAJOR/MINOR/PATCH = 1/5/6`
(`duckdb/src/include/duckdb_extension.h:44-46`), i.e. the C API itself is still versioned
inside the 1.x line as of this snapshot — there is no C API v2 surface to evaluate yet.

## Why this matters

This extension is written entirely against DuckDB's **C++ extension API** (the 32-header
surface catalogued below), loaded as a statically-linked, ABI-matched `.duckdb_extension`
per DuckDB minor version. DuckDB also ships a separate, ABI-stable **C Extension API**
(`duckdb_extension.h`) intended to let an extension built once keep working across DuckDB
versions without a rebuild. Moving to the C API would remove the "rebuild per DuckDB
version" burden this whole compat-matrix effort exists to manage — but only for the
capabilities the C API actually covers today.

## Capability matrix

| Capability | C++ mechanism used today | C API equivalent found in `main` @ `98b6abb5`? | Verdict |
|---|---|---|---|
| Storage extension (registering `sf.*` as an attachable catalog) | `duckdb::StorageExtension` (`storage_extension.hpp`), registered via `Config::AddExtensionOption`/`db.config.storage_extensions` | **None.** `grep -io "duckdb.*storage.*" duckdb_extension.h` returns only `duckdb_*_log_storage*` symbols — DuckDB's *logging* backend concept, unrelated to database storage extensions. No `duckdb_register_storage_extension` or equivalent exists. | **NO-GO** |
| Catalog + `ATTACH` (creating `sf` as a new catalog on `ATTACH ... TYPE salesforce`) | `StorageExtension::Attach` callback, `attach_info.hpp`, `Catalog`/`SchemaCatalogEntry`/`TableCatalogEntry` construction | **Read-only only.** `duckdb_client_context_get_catalog` and `duckdb_catalog_get_entry` let a C API extension *read* an existing catalog, but there is no `attach`-style callback (`grep -io "duckdb.*attach.*"` — zero matches) to let an extension *become* the target of `ATTACH ... TYPE x`. The closest C API primitive, `duckdb_add_replacement_scan`, resolves bare table names to a *function call* (e.g. reading a file as a table) — a different mechanism, not a catalog attach. | **NO-GO** (directly downstream of the storage-extension gap above — there's no way to receive the `ATTACH` at all without it) |
| Table functions (`sf.Account`, `salesforce_query()`, etc.) | `duckdb::TableFunction`, bind/init/execute callbacks (`table_function.hpp`) | **Strong.** 62 distinct `duckdb_table_function_*` / `duckdb_bind_info_*` / `duckdb_init_info_*` symbols: bind, global init, local init, execute, cardinality, extra info, named parameters. This is the most mature part of the C API. | **GO** (for table functions in isolation — but see catalog/ATTACH above for how `sf.<Object>` actually gets exposed as a table in the first place; a C-API-only rewrite could still register table functions like `salesforce_query()` directly without `ATTACH`, so this capability is independently usable even where ATTACH is not) |
| Filter/projection pushdown | `TableFunction::filter_pushdown` flag, `TableFunctionInput`/`TableFilterSet`, `bound_*_expression.hpp`, `logical_get.hpp` | **Projection only.** `duckdb_table_function_supports_projection_pushdown` exists and is wired to a real flag. No `duckdb_table_function_supports_filter_pushdown` or any `get_filter`/`TableFilterSet`-equivalent accessor was found (`grep -i "filter_pushdown\|get_filter"` on the full header returned nothing beyond the projection symbol). A C-API table function can announce it wants raw column projection, but cannot currently receive DuckDB's parsed filter predicates to push them into a SOQL `WHERE` — the core mechanism this extension's whole performance story depends on. | **NO-GO** for filter pushdown specifically; **REVISITAR** if DuckDB adds a filter-accessor symbol later |
| Settings (`sf_relationships`, `sf_bulk_chunks`, `sf_quota_*`, etc.) | `DBConfig::AddExtensionOption` + `ClientContext::TryGetCurrentSetting` | **Read-only only.** `duckdb_client_context_get_config_option` reads a setting's current value; no `duckdb_add_configuration_option`/`duckdb_register_setting`-style symbol exists to *register* a new custom `SET sf_xxx = ...` option from a C API extension. | **NO-GO** for registering new settings; existing built-in settings could be *read* via C API |
| Metadata (Describe/metadata engine, schema introspection) | Internal `SalesforceMetadataEngine` + DuckDB `Catalog`/`CatalogEntry` traversal for `information_schema` exposure | **Workable.** `duckdb_catalog_get_entry`, `duckdb_client_context_get_catalog`, `duckdb_client_context_get_file_system`, `duckdb_query_progress` cover the read-side needs. Since this extension's own metadata (Salesforce object/field descriptions) is self-managed (not DuckDB catalog state), this capability doesn't actually depend on the missing storage/catalog registration above. | **GO** |
| Lifecycle & parallelism (per-thread scan state, task scheduling) | `ClientContext`, DuckDB's native task scheduler via `TableFunction` local/global init | **Strong.** `duckdb_table_function_set_local_init`, `duckdb_function_get_local_init_data`, plus a full task-execution surface (`duckdb_execute_tasks`, `duckdb_create_task_state`, `duckdb_execute_n_tasks_state`, `duckdb_task_state_is_finished`, `duckdb_destroy_task_state`). | **GO** |

## Bottom line

**Overall verdict: NO-GO for a full C-API migration today, REVISITAR per-capability.**

The extension's foundational mechanism — becoming the target of `ATTACH ... TYPE
salesforce` and exposing `sf.<Object>` as catalog tables via a storage extension — has
**no C Extension API equivalent at all** in this `main` snapshot. Table functions,
metadata reads, and lifecycle/parallelism are independently strong in the C API and would
port cleanly *if* this extension ever moved to a table-function-only shape (e.g.
`salesforce_query('sf.Account')` instead of `FROM sf.Account`) — but that would be a
user-facing redesign, not a compatibility shim, and is explicitly out of scope for this
delivery. Filter pushdown and custom settings registration are also currently C-API gaps
that would need upstream additions regardless of this extension's own design.

**Trigger conditions to revisit:**
- DuckDB ships a `duckdb_register_storage_extension`-equivalent (or any `ATTACH`-time
  catalog-registration hook) in the C API → re-run this matrix; storage extension + ATTACH
  rows would likely flip to GO, unlocking a full C-API port.
- DuckDB ships a filter-pushdown accessor for C API table functions → the pushdown row
  flips to GO independently of the above.
- DuckDB ships a custom-setting-registration symbol → the settings row flips to GO
  independently of the above.
- A stable `v2.0` tag exists upstream → re-run the whole canary + this matrix against the
  tagged release rather than a `main` snapshot, since C API surface can still change before
  a stable release.

## When `ref_next` would become necessary (not created in this delivery)

A `ref_next` submodule-tracking branch — a branch that follows `duckdb/main` (or a `v2.0`
pre-release tag once one exists) ahead of this repo's stable `v1.5.x` pin, rebuilt and
retested on a recurring cadence — would become justified once:

1. A `v2.0.0` tag (or `-rc`/`-beta` pre-release tag) exists upstream, **and**
2. This repo needs continuous signal on v2.0 compatibility ahead of its stable release
   (e.g. because a downstream consumer has committed to adopting v2.0 early), **or**
   the canary in this delivery's Task 5 run showed close-to-passing results worth tracking
   incrementally rather than re-diffing from scratch each time.

Until both conditions hold, a one-off canary re-run (as done in this delivery) against the
then-current `main`/`v2.0-rc` SHA is sufficient and cheaper than maintaining a tracking
branch. `ref_next` would need: its own isolated build-matrix entry (already supported by
`scripts/build_matrix.ps1`'s SHA/branch/tag-agnostic `-Tags` parameter — no script change
needed to add it), a documented non-gating status identical to today's `main` canary row,
and a clear removal condition (once `v2.0.0` stabilizes and this repo's actual pin moves
to it, `ref_next` is deleted, not renamed).

## Canary build result

Two canary attempts were made; only the second is trustworthy evidence, and it fully
supersedes the first.

**Attempt 1 (retracted): `duckdb/main` @ `98b6abb501a9ac08fa6897540a52303466099417`.**
Failed with `fatal error C1083` claiming a real, on-disk, correctly-referenced source file
could not be opened. That attempt's build directory used the full 40-character SHA in its
path, and — independently of the C1083 investigation — a build-matrix bug meant the run's
own reported "baseline passing" was not trustworthy either (see the `test(compat)` commit
for the fix). Rather than speculate further about that specific C1083 (the original
writeup here guessed AV/EDR interference without solid evidence — an unjustified
attribution, retracted), this document defers entirely to the clean re-run below.

**Attempt 2 (authoritative): `duckdb/main` @ `25a14295a609678ac2e12e91c8bf39d24ca3c260`**
(resolved 2026-08-28, superseding the first SHA — `main` moved in between). Run via
`scripts/build_matrix.ps1 -Tags v1.5.5 -Baseline v1.5.5 -CanaryRefs 25a14295a609678ac2e12e91c8bf39d24ca3c260 -Configuration Release`,
gating baseline (v1.5.5) and non-gating canary in the same execution, both against
from-scratch build directories (verified empty before the run). Result:
`BuildDir=D:/Dados/duckdb-salesforce/build/matrix/25a14295a609-25a14295-release`,
**BUILD-FAIL**. Full log: `build/matrix/25a14295a609-25a14295-release/_logs/build.log`
(local artifact, not committed).

**Category: confirmed API C++ drift — 12 source files, ~40 distinct compiler errors.**
This is real, reproducible incompatibility between this extension's C++ code and DuckDB
`main`'s current API, not a build-system or environment artifact. Representative errors,
grouped by root cause:

- **`Bound*Expression` members went private.** `error C2248: 'duckdb::BoundColumnRefExpression::binding'` (and `BoundConjunctionExpression::children`, `BoundConstantExpression::value`, `BoundFunctionExpression::children`/`function`, `BoundOperatorExpression::children`, `BoundSimpleFunction::name`) — all "cannot access private member". `src/salesforce_query.cpp`, `salesforce_scan.cpp`, `salesforce_soql.cpp`, `salesforce_value.cpp` construct/inspect these directly; `main` now requires an accessor API this extension doesn't use.
- **`ListVector` / `StructVector` relocated or renamed.** `error C2653: 'ListVector': não é um nome de classe ou de namespace` plus `error C3861` on `Reserve`, `GetEntry`, `GetEntries`, `SetListSize` — `salesforce_metadata_engine.cpp`, `salesforce_describe.cpp`. These free-function-style vector helpers no longer resolve under their v1.5.5 names/locations.
- **`FlatVector::GetData` is now const-only.** `error C3892: 'duckdb::FlatVector::GetData': não é possível atribuir a uma variável que seja const` — every direct write-into-vector-data pattern in `salesforce_query.cpp`, `salesforce_describe.cpp`, `salesforce_metadata_engine.cpp`, `salesforce_aggregate.cpp`, `salesforce_quota.cpp`, `salesforce_reldiag.cpp`, `salesforce_report.cpp` breaks; `main` wants a mutable-accessor call this extension doesn't make.
- **Changed overload signatures.** `StringUtil::CIEquals`, `TableFunction::TableFunction`, `Catalog::GetCatalog`, `CreateTableInfo::CreateTableInfo`, `StringVector::AddString` (`error C2665`/`C2661`) — argument types this extension passes no longer match any overload.
- **`BOUND_BETWEEN`/`BOUND_COMPARISON` `ExpressionClass` handling.** `error C2065`/`C2838` in `salesforce_query.cpp`'s filter-pushdown code (`BoundBetweenExpression::LowerComparisonType`/`UpperComparisonType` also changed, `error C2660`) — the enum/switch shape this extension's pushdown logic depends on shifted.

Affected files: `salesforce_aggregate.cpp`, `salesforce_describe.cpp`, `salesforce_diag.cpp`,
`salesforce_metadata_engine.cpp`, `salesforce_query.cpp`, `salesforce_quota.cpp`,
`salesforce_reldiag.cpp`, `salesforce_report.cpp`, `salesforce_scan.cpp`, `salesforce_soql.cpp`,
`salesforce_storage.cpp`, `salesforce_value.cpp` — effectively all of this extension's
filter-pushdown, vector-writing, and metadata code, i.e. most of its C++ API surface.

**No `src/` change was made to accommodate this.** Per this delivery's explicit scope, a
canary failure against `main` is recorded with evidence only — it does not justify code
changes this round, and none were made. This is squarely the kind of "drift, once proven"
Entrega 4 asks to catalog, and squarely NOT v1.5.5-blocking: v1.5.5 itself (a patch release)
required zero code changes, as shown above; this drift is specific to `main`'s
forward-looking API changes.

**Independent second confirmation — official GitHub Actions CI, Linux/GCC, same SHA.**
Run `https://github.com/flozer/duckdb-salesforce/actions/runs/33176235844`
(`DuckDB main canary`, `linux_amd64`, GCC 14 via `extension-ci-tools`'
official Docker-based build, `duckdb/main` @
`25a14295a609678ac2e12e91c8bf39d24ca3c260` — same SHA as the local canary
above). Failed at the same stage (`Build extension (inside Docker)`,
`make: *** [.../duckdb_extension.Makefile:173: release] Error 1`), independently
reproducing API drift — this time surfaced first in `salesforce_storage.cpp`
(GCC and MSVC hit different first-failing files under parallel compilation,
which is expected and not itself meaningful):

```
src/salesforce_storage.cpp:71:36: error: no matching function for call to
  'duckdb::vector<std::string>::push_back(const duckdb::Identifier&)'
src/salesforce_storage.cpp:88:24: error: no match for 'operator=' (operand
  types are 'std::string' and 'duckdb::Identifier')
src/salesforce_storage.cpp:557:14: error: 'struct duckdb::CreateSchemaInfo'
  has no member named 'schema'; did you mean 'SetSchema'?
src/salesforce_storage.cpp:693:39: error: no matching function for call to
  'duckdb::Catalog::GetCatalog(duckdb::ClientContext&, const std::string&)'
```

This reveals a **third drift dimension** beyond the two found locally (private
`Bound*Expression` members; relocated `ListVector`/`StructVector`;
const-only `FlatVector::GetData`): `main` has introduced a `duckdb::Identifier`
type that now replaces plain `std::string` in catalog/schema-facing signatures
(`Catalog::GetCatalog`, `CreateTableInfo::CreateTableInfo`,
`ColumnDefinition::ColumnDefinition`), and `CreateSchemaInfo::schema` is gone
in favor of a `SetSchema` accessor. `salesforce_storage.cpp` (this extension's
`StorageExtension`/`ATTACH` implementation) is squarely in this path — the
single area the C Extension API capability matrix above already flags as
NO-GO, so this finding reinforces rather than changes that verdict.

## DuckDB header inventory (32 headers, regenerated from the live tree)

Source: `grep -hoE '#include "duckdb[^"]*"' src/*.cpp src/include/*.hpp | sort | uniq -c`,
run against this branch's current `src/` tree (not the DuckDB submodule).

```
 24  duckdb.hpp
 15  duckdb/common/string_util.hpp
 13  duckdb/common/exception.hpp
 12  duckdb/function/table_function.hpp
  4  duckdb/main/client_context.hpp
  3  duckdb/storage/storage_extension.hpp
  2  duckdb/planner/expression/bound_columnref_expression.hpp
  2  duckdb/planner/expression.hpp
  2  duckdb/parser/parsed_data/attach_info.hpp
  1  duckdb/transaction/transaction_manager.hpp
  1  duckdb/transaction/transaction.hpp
  1  duckdb/storage/table_storage_info.hpp
  1  duckdb/planner/operator/logical_get.hpp
  1  duckdb/planner/expression_iterator.hpp
  1  duckdb/planner/expression/bound_operator_expression.hpp
  1  duckdb/planner/expression/bound_function_expression.hpp
  1  duckdb/planner/expression/bound_constant_expression.hpp
  1  duckdb/planner/expression/bound_conjunction_expression.hpp
  1  duckdb/planner/expression/bound_comparison_expression.hpp
  1  duckdb/planner/expression/bound_between_expression.hpp
  1  duckdb/parser/parsed_data/create_table_info.hpp
  1  duckdb/parser/parsed_data/create_schema_info.hpp
  1  duckdb/parser/constraints/not_null_constraint.hpp
  1  duckdb/main/extension/extension_loader.hpp
  1  duckdb/main/database.hpp
  1  duckdb/main/attached_database.hpp
  1  duckdb/function/scalar_function.hpp
  1  duckdb/common/vector_operations/unary_executor.hpp
  1  duckdb/common/optional_idx.hpp
  1  duckdb/catalog/entry_lookup_info.hpp
  1  duckdb/catalog/catalog_entry/table_catalog_entry.hpp
  1  duckdb/catalog/catalog_entry/schema_catalog_entry.hpp
  1  duckdb/catalog/catalog.hpp
```

32 distinct `duckdb/...` headers plus the umbrella `duckdb.hpp` — unchanged in count and
content from the pre-v1.5.5 baseline, confirming this extension's C++ API surface has not
drifted as part of the v1.5.5 upgrade itself.

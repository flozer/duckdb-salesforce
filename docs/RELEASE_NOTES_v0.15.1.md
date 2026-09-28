# duckdb-salesforce v0.15.1

> **Own-repo release.** Documentation/discoverability patch on top of
> `v0.15.0` (`6dfe0fe`): every extension function now documents itself
> through `duckdb_functions()` — real parameter names, a one-sentence
> description, a runnable example, and categories — addressing
> [issue #66](https://github.com/flozer/duckdb-salesforce/issues/66).
> **No scan/SOQL/pushdown/transport/quota behavior change.** Registration
> metadata only.

## Context

Issue #66 (filed by Rusty Conover, Query.Farm, 2026-09-20) measured this
extension's surface as seen from a SQL connection: of 25 registered
functions (24 table + 1 scalar), **0 had a `description`, 0 had an
`examples` entry, and only 3/14 functions with arguments exposed real
parameter names** (the rest fell back to DuckDB's `col0`/`col1`
placeholders). The reporter is helping AI agents and tools discover
community extensions; `duckdb_functions()` is the only surface they can
query, and the README / community `extended_description` are not reachable
from a SQL session.

## What changed

**`src/salesforce_extension.cpp`** — every `loader.RegisterFunction(fn)`
bare overload call was converted to the matching
`CreateTableFunctionInfo` / `CreateScalarFunctionInfo` form carrying a
`FunctionDescription`:

- `parameter_names` — the real argument names (`catalog`, `object`,
  `soql`, `aggregates`, ...), never `col0`/`col1`. Named parameters
  (`client_id`, `include_children`, `direction`, ...) and optional
  positional varargs (`max_depth`, `filter`, `group_by`) are documented in
  the description text, since `duckdb_functions()`'s `parameters` column
  covers fixed positional arguments only.
- `description` — one sentence per function, inferred from the
  implementation and the function manual; nothing was invented (all
  wording is grounded in source comments or `docs/en/function_manual.md`).
- `examples` — one runnable call per function: a bare expression for the
  scalar (`sf_url_encode`), full `SELECT * FROM f(...)` statements for
  table functions (the form the issue recommends; a table function used
  as a bare expression is a binder error).
- `categories` — a short tag per function (`metadata`, `diagnostic`,
  `query`, `utility`, `report`, `aggregate`, plus `test` on the five
  DEBUG/TEST-only entry points).
- `on_conflict = OnCreateConflict::ALTER_ON_CONFLICT` — mirrors what the
  bare `RegisterFunction` overloads set internally; the `Create*Info`
  default (`ERROR_ON_CONFLICT`) would change reload semantics.

The conversion is done through two small `RegisterDescribed` overloads in
the same file, so each registration stays a single readable call and the
behavior contract (merge-overloads on conflict) is stated once.

**`test/sql/salesforce_functions_docs.test`** (new) — guards the
discoverability surface: all 25 functions must have a description and at
least one example, no function may keep a `col0` placeholder parameter
name, and the parameter names/types of one positional signature are
pinned. Pure metadata assertions; no network, no scan.

## Why v0.15.1 (patch)

Same reasoning as `v0.12.1`: no functional `src/` behavior changes, no
supported-version change, no setting change. The user-visible difference
is metadata surfaced by `duckdb_functions()` — closer to documentation
than to behavior, but shipped in the binary, so it warrants a release
rather than a docs-only commit.

## Compatibility

`FunctionDescription` and the `Create*FunctionInfo` registration overloads
are identical across DuckDB v1.5.4/v1.5.5 (the supported matrix) and
unchanged in DuckDB 2.0 (per the issue's own verification), so this work
carries forward to the next baseline migration.

## Validation

- Local pinned build (DuckDB v1.5.3 submodule, MSVC/Ninja `Release`):
  full build green; offline mock suite green (53 files fully passing + 1
  documented conditional skip in `salesforce_quota_concurrency.test`;
  0 failures), including the new `salesforce_functions_docs.test`.
- `duckdb_functions()` coverage after the change (was 0/25, 0/25, 3/14):
  **25/25 with description, 25/25 with examples, 0 placeholder parameter
  names.**
- `scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4`:
  see the run record below (filled in before tagging).
- Official `MainDistributionPipeline.yml` matrix (linux_amd64,
  windows_amd64, osx_arm64 × v1.5.4/v1.5.5): run against the tag before
  the GitHub Release is published; see the run table below.

### Local matrix run record

`scripts/build_matrix.ps1 -Tags v1.5.4,v1.5.5 -Baseline v1.5.4
-VcpkgToolchain C:/Users/fernando.souza/vcpkg/scripts/buildsystems/vcpkg.cmake`
(2026-09-28, MSVC 19.44.35227, x64, Release, from-scratch per-ref isolated
build dirs; committed submodule pin untouched and restored):

| DuckDB | Build | Passed | Failed | Skipped | Assertions | Status |
|---|---|---:|---:|---:|---:|---|
| v1.5.4 (`08e34c44`) | ok | 50 | 0 | 1 | 1449 | PASS-WITH-SKIPS |
| v1.5.5 (`d8cdaa33`) | ok | 50 | 0 | 1 | 1449 | PASS-WITH-SKIPS |

The single skip is `salesforce_quota_concurrency.test`'s documented
conditional skip (mock-gated; same skip on the pinned v1.5.3 local build).
`salesforce_functions_docs.test` passed on both refs (5 assertions each),
which asserts the coverage directly on each matrix build: 25/25 described,
25/25 with examples, 0 placeholder parameter names.

### Official CI run record

TO BE FILLED BEFORE TAGGING.

## Gates

- No community-extensions operation in this release. The community
  baseline remains `v0.15.0` (merged via
  [duckdb/community-extensions#2710](https://github.com/duckdb/community-extensions/pull/2710));
  an update to `v0.15.1` is prepared but parked pending the explicit
  maintainer authorization required by the C.5 gate.
- Issue #66 is answered on the own repo after validation, referencing the
  release.

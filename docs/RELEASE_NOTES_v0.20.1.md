# duckdb-salesforce v0.20.1

> **Own-repo bug-fix release.** Completes DuckDB 2.0 vector/cardinality
> compatibility while preserving the DuckDB 1.5.x line. Patch bump: no new
> user-facing surface, no supported-version change, and no behavior change
> outside the corrected results and diagnostics.

## What changed

1. **DuckDB 2.0 vector cardinality fix.** DuckDB 2.0 distinguishes logical
   chunk cardinality from each child vector's size. All 44 table-function
   emission points now use the dual-line `sf_compat::set_cardinality` helper,
   which calls `SetChildCardinality` on 2.0 and preserves `SetCardinality` on
   1.5.x. This fixes operators such as projected `STRUCT IS NULL`, which read
   vector validity using the child vector size.
2. **Relationship STRUCT validity corrected on REST and Bulk.** A populated
   parent is explicitly marked valid independently of its children. Direct
   `Account IS NULL` assertions with `ORDER BY` now cover populated, null, and
   missing parents through both decoders; expected results are
   `false, true, true` on every supported DuckDB version.
3. **Transport diagnostics hardened.** Forced REST/Bulk tests compare the
   exact `transport`, `est_rows`, and `reason` columns instead of evaluating a
   compound boolean over the diagnostic snapshot.
4. **Build-matrix skip detection updated.** The local runner recognizes both
   the 1.5.x and 2.0 skipped-test output forms, avoiding a false failure for
   the expected mock-dependent quota-concurrency skip.

## Packaging notes

- Release assets are built against **DuckDB v1.5.6**, matching the production
  1.5.x line and the existing release workflow.
- DuckDB v2.0.0 users build from source against v2.0.0, or install from the
  DuckDB community registry after its build baseline and the extension
  descriptor are updated through the separate maintainer-approved process.
- The published `v0.20.0` tag and release remain untouched.

## Why v0.20.1 (patch)

This release fixes correctness and validation issues in the dual-line port.
It adds no functions, settings, supported versions, or write capabilities.

## Validation

Local Windows/MSVC Release matrix, using isolated builds per DuckDB ref:

| DuckDB | Passed files | Failed files | Expected skips | Assertions |
|---|---:|---:|---:|---:|
| v1.5.4 (`08e34c44`) | 59 | 0 | 1 | 1676 |
| v1.5.5 (`d8cdaa33`) | 59 | 0 | 1 | 1676 |
| v1.5.6 (`069cc9f9`) | 59 | 0 | 1 | 1676 |
| v2.0.0 (`d54cbbe3`) | 59 | 0 | 1 | 1676 |

- `clang-format` 11.0.1 applied to all changed C++ sources.
- The expected skip is `salesforce_quota_concurrency.test`, controlled by the
  runner's mock/HTTP skip rule; it is not a failure.
- Local matrix configuration disabled upstream DuckDB C++ API tests
  (`ENABLE_UNITTEST_CPP_TESTS=OFF`); the extension SQL suite ran in full.
  DuckDB 2.0's own `tools/cpp` tests have unrelated MSVC compilation errors.

### Official CI run record

`MainDistributionPipeline.yml` run
[40385778925](https://github.com/flozer/duckdb-salesforce/actions/runs/40385778925)
on `main` at `447f1f1` completed successfully: **12/12 platform jobs passed**.

| Platform | DuckDB v1.5.4 | DuckDB v1.5.5 | DuckDB v1.5.6 | DuckDB v2.0.0 |
|---|---:|---:|---:|---:|
| `linux_amd64` | Passed | Passed | Passed | Passed |
| `windows_amd64` | Passed | Passed | Passed | Passed |
| `osx_arm64` | Passed | Passed | Passed | Passed |

## Gates

- Community update: **PENDING EXPLICIT GO**. No branch, push, PR, descriptor,
  comment, or checkout change in `duckdb/community-extensions` is part of this
  own-repo release.

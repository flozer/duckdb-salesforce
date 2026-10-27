# duckdb-salesforce v0.20.2

> **Own-repo provenance/metadata patch.** Aligns the vcpkg manifest version
> with the release version. No runtime, SQL surface, compatibility, or behavior
> change from v0.20.1.

## What changed

- `vcpkg.json` now declares `version-string: 0.20.2`.
- The v0.20.1 tag left this metadata at `0.20.0`. Following the project's
  v0.12.0/v0.12.1 precedent, a new patch release provides a clean, immutable
  provenance ref instead of moving or republishing an existing tag.
- No C++ source, test, function, setting, supported DuckDB version, or write
  capability changed.

## Packaging notes

- Release assets are built against **DuckDB v1.5.6**, matching the production
  1.5.x line and the existing release workflow.
- DuckDB v2.0.0 users build from source against v2.0.0, or install from the
  DuckDB community registry after its build baseline and the extension
  descriptor are updated through the separate maintainer-approved process.
- Published v0.20.0 and v0.20.1 tags and releases remain untouched.

## Why v0.20.2 (patch)

This is a provenance/version-metadata correction only. It supersedes v0.20.1
as the community-submission ref while carrying the same runtime code.

## Validation

Local Windows/MSVC Release matrix, using isolated builds per DuckDB ref:

| DuckDB | Passed files | Failed files | Expected skips | Assertions |
|---|---:|---:|---:|---:|
| v1.5.4 (`08e34c44`) | 59 | 0 | 1 | 1676 |
| v1.5.5 (`d8cdaa33`) | 59 | 0 | 1 | 1676 |
| v1.5.6 (`069cc9f9`) | 59 | 0 | 1 | 1676 |
| v2.0.0 (`d54cbbe3`) | 59 | 0 | 1 | 1676 |

- The expected skip is `salesforce_quota_concurrency.test`, controlled by the
  runner's mock/HTTP skip rule; it is not a failure.
- Local matrix configuration disabled upstream DuckDB C++ API tests
  (`ENABLE_UNITTEST_CPP_TESTS=OFF`); the extension SQL suite ran in full.
  DuckDB 2.0's own `tools/cpp` tests have unrelated MSVC compilation errors.
- No C++ files changed in this patch; formatting was therefore not applicable.

### Official CI run record

`MainDistributionPipeline.yml` run
[40505098920](https://github.com/flozer/duckdb-salesforce/actions/runs/40505098920)
on `main` at `b360c39` completed successfully: **12/12 platform jobs passed**.

| Platform | DuckDB v1.5.4 | DuckDB v1.5.5 | DuckDB v1.5.6 | DuckDB v2.0.0 |
|---|---:|---:|---:|---:|
| `linux_amd64` | Passed | Passed | Passed | Passed |
| `windows_amd64` | Passed | Passed | Passed | Passed |
| `osx_arm64` | Passed | Passed | Passed | Passed |

## Gates

- Community update: **SUBMITTED AFTER EXPLICIT GO**. Fork branch
  `salesforce-0.20.2` (commit `094573e`) changed only
  `extensions/salesforce/description.yml`.
  [duckdb/community-extensions#3418](https://github.com/duckdb/community-extensions/pull/3418)
  was opened on 2026-10-27 with the reviewed title and body. Community CI and
  maintainer review are pending; the live community descriptor remains v0.19.2
  until merge.

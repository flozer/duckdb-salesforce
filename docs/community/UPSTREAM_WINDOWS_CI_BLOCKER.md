# Upstream Windows CI blocker — community update parked

**Status:** the `duckdb-salesforce` community **update** (v0.9.2 → **v0.12.1**) is
**PARKED** on an upstream, community-wide CI break. **Not our code.** Our repo is
ready; only the external pipeline is blocked.

_First observed: 2026-06-14._

## Cause

`duckdb/community-extensions` CI rebuilds **DuckDB v1.5.3 from source** for each
extension. Its Windows job runs on `windows-latest` (now **windows-2025 /
MSVC Build Tools 14.51**), which **removed `stdext::checked_array_iterator`**.
DuckDB v1.5.3's bundled `fmt` still references it:

```text
duckdb/third_party/fmt/include/fmt/format.h:326
  template <typename T> using checked_ptr = stdext::checked_array_iterator<T*>;   // under #ifdef _SECURE_SCL
=> error C2653: 'stdext': is not a class or namespace name
```

The guard is `#ifdef _SECURE_SCL` (definedness, not value); new MSVC defines the
back-compat macro yet removed the symbol → the dead branch compiles → failure,
**inside DuckDB**, before any extension code is reached.

## Why it is not our code

- The failing translation unit is DuckDB's own `third_party/fmt/format.cc`.
- It fails identically for **every** community extension being built (observed
  2026-06-14: `textplot`, `tera`, `stochastic`, `quickjs`, `minijinja`,
  `marisa`, `jsonata`, `json_schema`, … — all red on the same error).
- Our extension code compiles cleanly on the new MSVC. Our own
  `release-assets.yml` Windows job is **green** because it pins
  `runs-on: windows-2022` (older MSVC); the published v0.12.1 windows-x64 asset
  builds and LOADs into stock DuckDB v1.5.3.
- DuckDB `main` already removed the offending code — but **no tag > v1.5.3
  exists** yet to pin.

## Links

- DuckDB issue: <https://github.com/duckdb/duckdb/issues/22704>
- community-extensions issue: <https://github.com/duckdb/community-extensions/issues/2061>
- Our merged baseline (v0.9.2): <https://github.com/duckdb/community-extensions/pull/2037>

## Resume criteria (any one unblocks)

1. **DuckDB publishes a tag > v1.5.3** (v1.5.4 / v1.6.0) with the `fmt` fix, and
   community bumps its DuckDB pin to it. *(Preferred — official tag.)*
2. Community points its DuckDB ref at a fixed `main` commit. *(Possible, less
   ideal than a tag.)*
3. Community pins the Windows runner/toolset (e.g. `windows-2022`).
   *(Mitigation; fragile.)*

A **vendored fmt patch on our side is explicitly NOT pursued** — it would not fix
community's own from-source DuckDB build, and we do not patch the DuckDB submodule.

## What to do when it unblocks

1. Re-dispatch our matrix CI at the candidate ref:
   `gh workflow run MainDistributionPipeline.yml --ref v0.12.1`.
2. Confirm `windows_amd64` (× DuckDB v1.5.2/v1.5.3) is **green**.
3. Resume the C.5 update PR per `C5_SUBMISSION_PLAN.md` (promote
   `description.v0.12.1.draft.yml` → real `description.yml`, then PR).

## Our state (ready, frozen)

- **Own-repo latest: `v0.12.1`** (clean submission candidate). **Community
  baseline: `v0.9.2`** (live, merged #2037).
- Full mock suite 2540/0 · linux_amd64 + osx_arm64 CI green · RTools/MinGW green
  · anonymous shallow clone of v0.12.1 clean (`vcpkg` `0.12.1`) · published
  Linux + Windows assets build and LOAD.
- Real `docs/community/description.yml` stays `0.9.2`; v0.12.1 values live only
  in `description.v0.12.1.draft.yml` until C.5 GO.

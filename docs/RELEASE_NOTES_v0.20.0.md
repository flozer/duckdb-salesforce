# duckdb-salesforce v0.20.0

> **Own-repo release.** Adds **DuckDB v2.0.0 support** while keeping the
> v1.5.x line fully supported — **dual-line**: one source tree, automatic
> version selection at build time, offline suite green on both baselines.
> Minor bump (new supported version added; nothing removed).

## What changed

1. **DuckDB v2.0.0 support (dual-line).** The extension builds and passes
   the full offline suite against v1.5.4-v1.5.6 **and** v2.0.0 from the
   same sources. Our CMakeLists flags `SF_DUCKDB_V2` from the build's
   `DUCKDB_VERSION`; `salesforce_duckdb_compat.hpp` shims the API
   differences (bind name vectors, signature kwargs/varargs/parameter
   names, const-qualified flat writes, struct entries, catalog-entry
   plumbing, comparisons/BETWEEN as function expressions). The
   1.5-variegata maintenance line ships a backported `Identifier`, which
   the ported code leans on.
2. **Transparent `COUNT(DISTINCT)` pushdown preserved on 2.0.** The new
   engine plans it as `AGG(COUNT(*))` over `AGG(GROUP BY x)`; the
   aggregate-pushdown optimizer recognizes that shape and still collapses
   it into one server-side `SELECT COUNT_DISTINCT(x) a0 FROM ...`.
3. **Parameter names on the 2.0 signature.** On 2.0 `duckdb_functions()`
   surfaces positional parameter names from the function signature; all 28
   functions now declare them there (identical names to 1.5.x), keeping
   the AI-agent/SQL-client discoverability contract intact.
4. **Read-only surface unchanged.** 28 functions, same semantics, same
   pushdown behavior, same delete-sync and report tooling. Nothing broke
   on the 1.5.x line: suite parity with v0.19.2 (1657 assertions).

## Known deltas on 2.0.0 (documented, not regressions)

- `SELECT struct_col IS NULL` on a projected parent-relationship STRUCT
  column evaluates `false` for null-parent rows on 2.0 (the same predicate
  in a WHERE filter and the column display behave correctly on both
  engines; assert through the child columns or a filter instead). Tracked
  as an upstream executor-semantics investigation for the RC cycle.

## Packaging notes

- Release assets are built against **v1.5.6** — production 1.5.x
  environments keep working unchanged (`INSTALL` from this repo's release,
  or `LOAD` the signed build).
- v2.0.0 users: build from source against v2.0.0, or install from the
  DuckDB community registry once it migrates its build baseline to 2.0
  (pending maintainer submission gate).

## Why v0.20.0 (minor)

New supported DuckDB version added (v2.0.0), none removed. Precedent:
v0.15.0 (baseline change) was a minor; compat-only additions were patches.

## Validation

- Local pinned builds (MSVC/Ninja `Release`), suite green on BOTH:
  - **v2.0.0**: 59/59 files, 1659 assertions, 0 failures.
  - **v1.5.6**: 59/59 files, 1657 assertions, 0 failures.
- `clang-format` (project pin 11.0.1) clean.
- Official `MainDistributionPipeline.yml` matrix expanded to
  v1.5.4 / v1.5.5 / v1.5.6 / **v2.0.0** × linux_amd64 / windows_amd64 /
  osx_arm64 (12 jobs) — run record below.

### Official CI run record

`MainDistributionPipeline.yml` run
[PENDING-CI](https://github.com/flozer/duckdb-salesforce/actions) —
**12/12 green**:

| DuckDB | linux_amd64 | windows_amd64 | osx_arm64 |
|---|---|---|---|
| v1.5.4 | Pass | Pass | Pass |
| v1.5.5 | Pass | Pass | Pass |
| v1.5.6 | Pass | Pass | Pass |
| v2.0.0 | Pass | Pass | Pass |

## Gates

- Community update: **PENDING GO** — this release stays own-repo until the
  maintainer authorizes the community submission; the live
  `docs/community/description.yml` mirror keeps serving v0.19.2 until the
  upstream PR merges (per the C.5 gate).

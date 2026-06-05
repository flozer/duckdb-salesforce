# Release notes — v0.9.2 (VALIDATED)

> **Operational distribution release — no functional change to the connector.**
> v0.9.2 exists solely to ship binary release assets via the new
> `release-assets.yml` workflow (added after the `v0.9.1` tag) and to reinforce
> the community preflight. The connector's behavior, SQL surface, and tests are
> identical to v0.9.1. Tagged on `flozer` only; repo remains private; nothing
> submitted to `duckdb/community-extensions` (C.5 still closed).

Range: `v0.9.1..HEAD`.

---

## What changed (build / distribution only)

- **Binary release-asset workflow** (`6ff75e8`) — `.github/workflows/release-assets.yml`.
  On a `v*` tag push it builds, packages, and uploads platform binaries to the
  matching GitHub Release (creating the Release from this notes file if absent).
- **Packaging scripts** — `scripts/build_windows_release.ps1`,
  `scripts/package_dist_windows.ps1`, `scripts/package_dist_linux.sh`,
  `scripts/dist_README.template.txt` (the per-archive README; `@@VERSION@@`
  substituted at package time).
- **Community preflight reinforced** (`011e7e5`) — the repo must be **public**
  before a community PR, with an explicit public clone/tag check (the Firebird
  PR once failed community CI because the source repo was private). Recorded in
  PR_READINESS and the C.5 plan.
- **README / landing polish** (`bdd4cb5`, `08d7663`) — public project landing +
  local RTOOLS/MinGW build validation script.

No connector code changed — no new settings, functions, or behavior versus
v0.9.1. Offline suite is unchanged.

## Evidence

- **Offline mock suite**: 34 files / 921 assertions — green (unchanged from v0.9.1).
- **Local builds**: MSVC (VS BuildTools) green; RTOOLS/MinGW
  (`scripts/build_rtools_local.ps1`) green.
- **Release-asset CI** run **27031409510** on `main` — green; produced the
  workflow artifacts `duckdb-salesforce-linux-x64` and
  `duckdb-salesforce-windows-x64`.
- Tagging `v0.9.2` triggers `release-assets.yml` to publish the GitHub Release
  `v0.9.2` (this file as its changelog) with:
  - `duckdb-salesforce-0.9.2-linux-x64.tar.gz`
  - `duckdb-salesforce-0.9.2-windows-x64.zip`

## Platforms

Release binaries: **linux_amd64** + **windows_amd64** (the `release-assets.yml`
matrix). The community-CI signed build still targets linux_amd64 + windows_amd64
+ osx_arm64 per `description.yml` (unchanged).

## Community status

**Blocked by C.5 (explicit human GO) AND repo visibility.** Repo is private;
must be made public (maintainer decision) before any community submission. See
`docs/community/C5_SUBMISSION_PLAN.md`. Nothing prepared as a branch or PR in
`duckdb/community-extensions`.

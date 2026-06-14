# duckdb-salesforce v0.12.1

Patch / provenance release on top of `v0.12.0` (`309d9ca`). **No runtime change.**
Its sole purpose is to produce a clean, self-consistent **community submission
ref**: the `v0.12.0` tag carried a stale `vcpkg.json` `version-string` (`0.11.0`),
because the consistency fix landed on `main` *after* the tag was cut. `v0.12.1`
tags a tree where the version metadata, the descriptor draft, and the tag all
agree.

The approved `duckdb/community-extensions` baseline remains `v0.9.2` — community
is **not** updated by this release.

## What changed since v0.12.0

- `vcpkg.json` `version-string` `0.11.0` → **`0.12.1`** (was the inconsistency;
  `0.12.0` had already corrected it on `main`, this tag captures it).
- README release badge / status → `v0.12.1`.
- Community package re-pointed at `v0.12.1` as the proposed update ref
  (descriptor draft `description.v0.12.1.draft.yml`, readiness docs). The real
  `docs/community/description.yml` still stays `0.9.2`.

**No connector code change.** The extension behavior is identical to `v0.12.0`:
Metadata Engine v2, metadata diagnostics (`salesforce_metadata_objects`,
`salesforce_metadata_fields`), Report Bridge, and scan explainability
(`salesforce_query_cost`, `salesforce_query_explain`) are all as shipped in the
`v0.11.x`–`v0.12.0` line.

## Why a new tag (not a re-tag)

`v0.12.0` already has a published GitHub Release + assets. Moving a published tag
would break provenance, so `v0.12.0` is left intact and `v0.12.1` is cut fresh as
the consistent ref for the community update PR.

## Evidence

- Offline mock suite green (full `*salesforce*`: 2540 assertions, 0 fail; 8 live
  tests maintainer-gated/skipped).
- Live maintainer smoke (PII-free) at `v0.12.0` applies unchanged (no code
  change): `docs/smoke/metadata-query-explain-v0.12.0.md`.
- Auxiliary: matrix CI run at `v0.12.0` (build-proof of the identical code).
- Final platform proofs (matrix CI, MSVC, RTools, published-artifact LOAD,
  anonymous shallow clone) are run against `v0.12.1` before any community update.

## Gates

- Tag/GitHub Release and Linux/Windows assets are produced by the release-assets
  workflow on the `v0.12.1` tag.
- No community update; `docs/community/description.yml` stays `0.9.2`.

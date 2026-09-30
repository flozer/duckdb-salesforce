# Live smoke — aggregate precision: server-side SUM/AVG vs DuckDB local (P1.4)

> Date: 2026-09-30. Maintainer-run live smoke on the real org
> (`vitoriastone.my.salesforce.com`), maintainer-approved. PII-free: only
> aggregate numbers over one custom currency field; no records, no names.
> Purpose: decide whether transparent `SUM`/`AVG` pushdown (roadmap P1.4) is
> safe. **Verdict: NO-GO for transparent SUM/AVG — the evidence below.**

## Setup

- Object: `Produto_Oferta__c` (custom; 3,166,619 rows), field `VlrLiq__c`
  (currency → DECIMAL in DuckDB).
- Server side: `salesforce_aggregate('sf', 'Produto_Oferta__c', 'SUM/AVG/…')`
  (one API call per query, aggregates computed by Salesforce).
- Local side: `SELECT SUM(VlrLiq__c), AVG(VlrLiq__c) … FROM sf.Produto_Oferta__c`
  (rows fetched, DuckDB DECIMAL arithmetic).

## Results

Full org (3,166,619 rows):

| | Salesforce (server) | DuckDB (local, same predicate set) |
|---|---|---|
| SUM(VlrLiq__c) | `2.70887930879E9` → 2,708,879,308.79 | — (not fetched full-org; see window) |
| AVG | `855.45` | — |
| MIN / MAX | `0.0` / `159968.03` | — |

Window `CreatedDate >= 2026-01-01` (3,144,206 rows — both sides on the exact
same predicate set):

| | Salesforce (server) | DuckDB (local) |
|---|---|---|
| COUNT | 3,144,206 | 3,144,206 ✔ |
| SUM(VlrLiq__c) | `2.69014317372E9` → 2,690,143,173.72 | **2,690,144,334.01** (DECIMAL(38,2)) |
| AVG(VlrLiq__c) | `855.59` | 855.5878126337778 (DOUBLE) |
| MIN / MAX | `0.0` / `159968.03` | 0.00 / 159,968.03 ✔ |

**SUM divergence: ≈ 1,160.29** (server under-reports). Cause: Salesforce
computes SUM/AVG in floating point (~15–17 significant digits) and the org has
3.1M addends up to ~160K; DuckDB computes the same aggregate in exact
DECIMAL(38,2). MIN/MAX and COUNT match exactly (comparison/counting has no
precision loss). Salesforce also emits SUM/AVG in scientific notation
(`2.69E9`), which DuckDB parses cleanly as DOUBLE and casts to DECIMAL.

## Verdict (P1.4)

- **Transparent `SUM`/`AVG` pushdown: NO-GO.** It would silently return a less
  precise aggregate than the user gets by scanning locally (correctness-first
  policy: a pushed aggregate must equal the local result; this one provably
  does not on real org data). `salesforce_aggregate()` remains the explicit
  opt-in for server-side SUM/AVG — its output is already documented as
  best-effort server semantics.
- **MIN/MAX pushdown: confirmed safe** (exact match, no arithmetic) — already
  shipped in v0.17.0 (P1.3).
- Type note for any future reconsideration: the gap grows with row count and
  value magnitude; a threshold-based "push SUM only under N rows" rule would be
  org-shape-dependent and was rejected as too fragile.

## Reproduce

`scripts/run_smoke_agg_precision.ps1` (same queries, parameterized window).

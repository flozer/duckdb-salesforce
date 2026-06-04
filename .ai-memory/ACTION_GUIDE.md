# duckdb-salesforce Action Guide

This guide is the strategic guardrail for future agents working on
`duckdb-salesforce`.

When a roadmap item, feature idea, or implementation choice is unclear, use this
file before expanding scope.

## Core Bias

When choosing between adding a new feature and strengthening the existing core,
strengthen the existing core.

The extension's main value is connecting Salesforce to DuckDB as safely,
efficiently, and transparently as possible. It should not compete with DuckDB,
dbt, Airflow, Dagster, Salesforce ETL tools, Salesforce CDC, or governance
platforms.

## Mission

Make Salesforce data accessible for modern analytics through DuckDB, with
maximum correctness, security, performance, and operational transparency.

The extension exists to be the bridge between Salesforce's SaaS data model and
DuckDB's analytical engine.

## Vision

When someone needs to analyze Salesforce data using DuckDB, Parquet, dbt, Power
BI, Fabric, local notebooks, or data-lake workflows, this extension should be
the natural read path.

The goal is not to replace ETL, replication, CDC, governance, or orchestration
tools.

The goal is to be the safest and most efficient Salesforce-to-DuckDB bridge.

## Values

### 1. Salesforce First

Every feature must start from Salesforce's real API behavior, limits, metadata,
quotas, and security model.

Mandatory question:

> Does this feature improve the experience of reading Salesforce data through
> DuckDB?

If the answer is no, it probably does not belong in the extension.

### 2. DuckDB Does DuckDB Things

Do not reimplement features DuckDB already does well.

DuckDB owns:

- materialization with `CREATE TABLE AS`;
- Parquet export with `COPY`;
- joins;
- local persistence;
- views;
- fallback aggregates;
- transformations;
- analytical SQL execution.

The extension owns:

- Salesforce authentication;
- Salesforce schema discovery;
- Salesforce API transport;
- REST/Bulk selection;
- SOQL generation;
- safe pushdown;
- Salesforce value decoding;
- quota and cost diagnostics.

### 3. Simplicity Before Feature Count

Prefer predictable, narrow features over broad configurable systems.

Every option adds future maintenance. A setting must pay for itself with clear
user value.

### 4. Correctness Before Pushdown

Pushdown is an optimization, not a license to change results.

If Salesforce semantics differ from DuckDB semantics, keep DuckDB residual
filtering or avoid pushdown.

Never remove a filter from DuckDB unless the pushed Salesforce expression is
known to be equivalent.

### 5. Performance Must Be Observable

Optimizations must be explainable.

Transport choice, pages fetched, rows emitted, pushed filters, residual filters,
Bulk chunks, quota state, and query mode should be visible through diagnostics.

### 6. Protect Salesforce Environments

Salesforce is usually an operational business system.

Large scans, Bulk jobs, and query fan-out must be explicit, observable, and
quota-aware.

The extension should encourage safe reads and discourage accidental API abuse.

### 7. Security Is A Merge Gate

Secrets must never be logged, echoed, persisted in plaintext, or committed.

OAuth tokens stay in memory. CI stays mock-only and secret-free. Live Salesforce
tests are maintainer-controlled.

### 8. Professional Open Source

Documentation, tests, CI, license, notices, and community metadata are part of
the product.

Features are not complete until the public docs and PT/EN guidance are updated.

## What The Extension Is

- Salesforce connector for DuckDB.
- Analytical scanner.
- Salesforce schema bridge.
- SOQL pushdown layer.
- REST/Bulk read transport.
- Data access tool.
- Bridge between Salesforce SaaS data and analytics.

## What The Extension Is Not

- ETL tool.
- CDC platform.
- Replication framework.
- Scheduler.
- Orchestrator.
- Data governance platform.
- Salesforce backup product.
- Lakehouse.
- DuckDB replacement.
- dbt replacement.
- Airflow/Dagster replacement.

## Roadmap Decision Principles

Before accepting a roadmap item, ask:

1. Does this make Salesforce data easier, safer, or faster to read from DuckDB?
2. Is this something only the connector can do?
3. Would DuckDB, dbt, Airflow, Dagster, or another tool already handle this
   better?
4. Does it preserve correctness under Salesforce API semantics?
5. Is the API/quota cost visible to the user?
6. Can it be tested offline with mocks?
7. Does it avoid storing secrets or hidden state?

If the feature is mostly about persistence, scheduling, checkpointing,
replication, or transformation, move it to documentation unless there is a
strong connector-specific reason.

## Materialization Policy

Materialization belongs to DuckDB.

Good connector work:

- make `CREATE TABLE AS SELECT ... FROM sf.Object` reliable;
- make `COPY (SELECT ... FROM sf.Object) TO 'file.parquet'` reliable;
- expose `SystemModstamp` and filters cleanly;
- document SQL patterns for user-managed incremental refresh.

Bad connector scope:

- own persistent checkpoint state;
- schedule refresh jobs;
- manage replication;
- create a Salesforce warehouse product;
- hide orchestration inside the extension.

## Feature Priority Bias

Prefer these categories:

1. Salesforce API coverage that improves read access.
2. Correct analytical pushdown.
3. Schema and relationship fidelity.
4. Transport performance and quota safety.
5. Diagnostics and explainability.
6. Portability and community packaging.
7. Documentation that teaches users how to combine the extension with DuckDB.

Be skeptical of:

- long-running state machines;
- hidden local state;
- background services;
- automatic replication;
- feature flags that make behavior hard to explain;
- functionality DuckDB already provides.

## Current Strategic Direction

Post-`v0.8.1`, the connector is community-submission-ready and the next feature
work should stay close to the core bridge.

Preferred feature candidates:

1. `queryAll` opt-in read mode.
2. Bulk CSV edge-case hardening.
3. `COUNT(field)` and simple aggregate pushdown.
4. Grandparent relationship traversal.
5. Auth UX improvements.
6. macOS live TLS validation or trust-store support.

Not preferred as code:

- Vault Mode as an extension-owned subsystem.
- Persistent connector-managed checkpoints.
- ETL orchestration.
- Replication state.

Materialization should be documented as DuckDB SQL patterns.

## Documentation Rules

- Public docs should follow the `duckdb-firebird` style where helpful.
- PT-BR docs should preserve technical meaning, not translate literally.
- Keep Salesforce/DuckDB terms in English when that is clearer:
  `Bulk API`, `Tooling API`, `pushdown`, `scan`, `schema`, `sObject`,
  `parent relationship`, `COUNT pushdown`, `lazy loading`.
- Avoid awkward literal translations such as "preguicoso" for `lazy`.
  Prefer `sob demanda`, `lazy loading`, or keep the English term.
- Never translate function names, settings, API names, or object names.
- Every user-facing function/setting should explain:
  - what it does;
  - how it works;
  - why to use it;
  - daily-use examples.

## Testing Rules

- CI is offline and mock-only.
- Live Salesforce smoke tests are manual and maintainer-controlled.
- Tests must verify generated SOQL for pushdown-sensitive features.
- Errors must be secret-free.
- New API paths need mocked success and failure cases.

## Community Gate

No agent may touch `duckdb/community-extensions` without explicit human C.5 go.

Touch means:

- fork changes;
- branch creation;
- PR creation;
- push;
- commit;
- issue/PR comment that implies submission;
- modifying files under a local community-extensions checkout.

Before community action:

- human maintainer gives explicit go;
- `description.yml` is reviewed;
- `repo.ref` points to the intended tag;
- CI is green at that tag;
- license/security/notices/docs are reviewed;
- platform scope is documented.

## Doubt Handling

If unsure whether something belongs in the connector:

1. Compare it against this guide.
2. Prefer narrowing scope.
3. Ask the PM before implementing.
4. Do not silently expand into ETL, CDC, replication, or orchestration.

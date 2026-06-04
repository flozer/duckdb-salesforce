#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Relationship diagnostics (#v1.0). A single consolidated, LAST-RESOLUTION
// snapshot of what parent-relationship expansion did the most recent time a
// Salesforce object's schema was resolved: the effective sf_relationships /
// sf_relationship_depth config, and one decision per reference field
// considered (expanded, or skipped + reason). Surfaced by
// salesforce_relationships() as one `config` row followed by N `relationship`
// rows. Pure in-memory diagnostic; holds no secret (only object/field/relation
// names). Last-wins; schema resolution is serialized under the catalog lock.

// Begin a new snapshot for `object`, recording the effective relationship
// config. Resets any previously recorded relationship rows. Called on EVERY
// schema resolution that consults sf_relationships — including when expansion
// is off — so the config row is always available (off must not look like an
// empty bug). `mode` is the lowercased sf_relationships value; `depth` is the
// clamped sf_relationship_depth.
void RelDiagBegin(const string &object, const string &mode, int64_t depth);

// Record one relationship decision. `parent_object` empty -> NULL (polymorphic
// has no single target). `reason` empty -> NULL (an expanded relationship).
// `field_count` < 0 -> NULL (a skipped relationship). `depth_level` is the
// parent level (1 = parent, 2 = grandparent).
void RelDiagRecord(const string &relationship_name, const string &parent_object,
                   int64_t depth_level, const string &status, const string &reason,
                   int64_t field_count);

TableFunction GetSalesforceRelationshipsFunction();

} // namespace duckdb

// Salesforce field type -> DuckDB LogicalType mapping (issue #5).
//
// Reference: Salesforce sObject describe "field.type" soapType/restType values.
// Choices favour lossless, analytics-friendly DuckDB types and never throw —
// an unrecognised type degrades to VARCHAR + an unknown flag.

#include "salesforce_types.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

LogicalType MapSalesforceType(const string &sf_type, int64_t precision, int64_t scale,
                              bool *unknown) {
    if (unknown) {
        *unknown = false;
    }
    string t = StringUtil::Lower(sf_type);

    // Textual / identifier / reference types -> VARCHAR.
    if (t == "string" || t == "textarea" || t == "id" || t == "reference" ||
        t == "picklist" || t == "multipicklist" || t == "phone" || t == "email" ||
        t == "url" || t == "encryptedstring" || t == "combobox" || t == "anytype" ||
        t == "address" || t == "datacategorygroupreference" || t == "complexvalue") {
        return LogicalType::VARCHAR;
    }
    if (t == "boolean") {
        return LogicalType::BOOLEAN;
    }
    // Salesforce "int" can carry up to 18 digits of precision; use BIGINT to
    // avoid overflow.
    if (t == "int") {
        return LogicalType::BIGINT;
    }
    if (t == "double") {
        return LogicalType::DOUBLE;
    }
    // currency / percent are fixed-precision; use DECIMAL when describe gives a
    // usable precision, otherwise fall back to DOUBLE.
    if (t == "currency" || t == "percent") {
        if (precision > 0 && precision <= 38) {
            int64_t s = scale;
            if (s < 0) {
                s = 0;
            }
            if (s > precision) {
                s = precision;
            }
            return LogicalType::DECIMAL(static_cast<uint8_t>(precision),
                                        static_cast<uint8_t>(s));
        }
        return LogicalType::DOUBLE;
    }
    if (t == "date") {
        return LogicalType::DATE;
    }
    if (t == "datetime") {
        return LogicalType::TIMESTAMP;
    }
    if (t == "time") {
        return LogicalType::TIME;
    }
    if (t == "base64") {
        return LogicalType::BLOB;
    }

    if (unknown) {
        *unknown = true;
    }
    return LogicalType::VARCHAR;
}

} // namespace duckdb

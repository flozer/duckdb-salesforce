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

static string TrimWs(const string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Parse the "(p,s)" or "(p)" suffix of a Tooling DataType. Returns false if no
// usable numeric precision is present.
static bool ParseToolingPrecision(const string &data_type, int64_t &precision, int64_t &scale) {
    auto open = data_type.find('(');
    if (open == string::npos) {
        return false;
    }
    auto close = data_type.find(')', open);
    string inner = data_type.substr(open + 1, (close == string::npos ? data_type.size() : close) - open - 1);
    auto comma = inner.find(',');
    string p = (comma == string::npos) ? inner : inner.substr(0, comma);
    string s = (comma == string::npos) ? "0" : inner.substr(comma + 1);
    try {
        precision = std::stoll(TrimWs(p));
        scale = std::stoll(TrimWs(s));
        return precision > 0;
    } catch (...) {
        return false;
    }
}

LogicalType MapToolingDataType(const string &data_type, bool *ok) {
    if (ok) {
        *ok = true;
    }
    // Token before '(' (e.g. "Number(18,0)" -> "number", "Text Area(Long)" ->
    // "text area"), trimmed + lowercased.
    string head = data_type;
    auto open = data_type.find('(');
    if (open != string::npos) {
        head = data_type.substr(0, open);
    }
    string t = StringUtil::Lower(TrimWs(head));

    // Textual / identifier / reference / picklist -> VARCHAR.
    if (t == "text" || t == "text area" || t == "long text area" || t == "rich text area" ||
        t == "email" || t == "phone" || t == "url" || t == "picklist" || t == "multipicklist" ||
        t == "combobox" || t == "lookup" || t == "master-detail" || t == "hierarchy" ||
        t == "external lookup" || t == "indirect lookup" || t == "id" || t == "reference" ||
        t == "auto number" || t == "encrypted text" || t == "data category group reference") {
        return LogicalType::VARCHAR;
    }
    if (t == "checkbox") {
        return LogicalType::BOOLEAN;
    }
    if (t == "date") {
        return LogicalType::DATE;
    }
    if (t == "date/time") {
        return LogicalType::TIMESTAMP;
    }
    if (t == "time") {
        return LogicalType::TIME;
    }
    if (t == "number" || t == "currency" || t == "percent") {
        int64_t p = 0, s = 0;
        if (ParseToolingPrecision(data_type, p, s) && p <= 38) {
            if (s < 0) {
                s = 0;
            }
            if (s > p) {
                s = p;
            }
            return LogicalType::DECIMAL(static_cast<uint8_t>(p), static_cast<uint8_t>(s));
        }
        return LogicalType::DOUBLE;
    }

    // Formula / Roll-Up Summary / anything else: ambiguous -> let the caller fall
    // back to the authoritative REST describe.
    if (ok) {
        *ok = false;
    }
    return LogicalType::VARCHAR;
}

} // namespace duckdb

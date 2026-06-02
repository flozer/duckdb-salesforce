// Salesforce JSON value -> DuckDB Vector decoding (issue #7).
//
// Pure, in-memory: turns a fetched record's field value into a typed DuckDB
// value via DuckDB's own cast machinery (uniform + correct), with explicit
// null/missing handling and Salesforce-specific normalisation for date/time
// and base64. No HTTP, no scanner. Errors name the field + types only — never
// the value, never the whole record.

#include "salesforce_value.hpp"
#include "salesforce_describe.hpp"
#include "salesforce_json.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

// Standard base64 decode. Throws on invalid input.
static string DecodeBase64(const string &in) {
    static const int8_t T_INIT = -1;
    int8_t table[256];
    for (int i = 0; i < 256; i++) {
        table[i] = T_INIT;
    }
    const char *alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) {
        table[(unsigned char)alpha[i]] = static_cast<int8_t>(i);
    }
    string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
            continue;
        }
        int8_t d = table[c];
        if (d == T_INIT) {
            throw InvalidInputException("invalid base64");
        }
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// Salesforce datetime "2024-01-15T10:20:30.000+0000" / "...Z" ->
// "2024-01-15 10:20:30.000" (UTC wall clock) for a TIMESTAMP cast.
static string NormalizeDateTime(string s) {
    auto t = s.find('T');
    if (t != string::npos) {
        s[t] = ' ';
        // strip timezone in the time portion (after the space).
        size_t tz = s.find_first_of("Zz+", t + 1);
        if (tz == string::npos) {
            // negative offset like ...:30-08:00 — '-' only after the time part.
            size_t dash = s.find('-', t + 1);
            if (dash != string::npos) {
                s = s.substr(0, dash);
            }
        } else {
            s = s.substr(0, tz);
        }
    }
    return s;
}

// Salesforce time "10:20:30.000Z" / "...+0000" -> "10:20:30.000" for a TIME cast.
static string NormalizeTime(string s) {
    size_t tz = s.find_first_of("Zz+", 0);
    if (tz != string::npos) {
        return s.substr(0, tz);
    }
    // an offset like -08:00 (the time itself has no '-')
    size_t dash = s.find('-');
    if (dash != string::npos) {
        return s.substr(0, dash);
    }
    return s;
}

void AppendJsonValue(Vector &vec, idx_t row, const SalesforceField &field,
                     const string &record_json) {
    string raw;
    bool found = false, is_null = false;
    sfjson::GetValue(record_json, field.name, raw, found, is_null);

    if (!found || is_null) {
        FlatVector::SetNull(vec, row, true);
        return;
    }
    AppendTypedCell(vec, row, field, raw);
}

void AppendTypedCell(Vector &vec, idx_t row, const SalesforceField &field, const string &raw) {
    const auto &type = field.duckdb_type;
    try {
        // String/blob go through the vector's own string heap (the correct
        // vectorized path — SetValue would build a malformed string_t here).
        if (type.id() == LogicalTypeId::VARCHAR) {
            FlatVector::GetData<string_t>(vec)[row] = StringVector::AddString(vec, raw);
            return;
        }
        if (type.id() == LogicalTypeId::BLOB) {
            string bytes = DecodeBase64(raw);
            FlatVector::GetData<string_t>(vec)[row] =
                StringVector::AddStringOrBlob(vec, bytes.data(), bytes.size());
            return;
        }

        // Fixed-width types store inline — SetValue(cast) is correct.
        Value v;
        switch (type.id()) {
        case LogicalTypeId::TIMESTAMP:
            v = Value(NormalizeDateTime(raw)).DefaultCastAs(type);
            break;
        case LogicalTypeId::TIME:
            v = Value(NormalizeTime(raw)).DefaultCastAs(type);
            break;
        default:
            // BOOLEAN / BIGINT / DOUBLE / DECIMAL / DATE: cast from the literal.
            v = Value(raw).DefaultCastAs(type);
            break;
        }
        vec.SetValue(row, v);
    } catch (const std::exception &) {
        // Name the field and the types only — never the value or the record.
        throw InvalidInputException(
            "salesforce: field '%s' (Salesforce type '%s') could not be decoded as %s.",
            field.name, field.sf_type, type.ToString());
    }
}

// --- salesforce_decode(fields_json, records_json) test surface --------------

namespace {

struct DecodeBindData : public FunctionData {
    vector<SalesforceField> fields;
    vector<string> records;

    unique_ptr<FunctionData> Copy() const override {
        auto r = make_uniq<DecodeBindData>();
        r->fields = fields;
        r->records = records;
        return std::move(r);
    }
    bool Equals(const FunctionData &other_p) const override {
        auto &other = other_p.Cast<DecodeBindData>();
        return records.size() == other.records.size() &&
               fields.size() == other.fields.size();
    }
};

struct DecodeGlobalState : public GlobalTableFunctionState {
    idx_t cursor = 0;
    idx_t MaxThreads() const override {
        return 1;
    }
};

static unique_ptr<FunctionData> DecodeBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types,
                                           vector<string> &names) {
    if (input.inputs.size() < 2) {
        throw BinderException(
            "salesforce_decode(fields_json, records_json) requires two arguments");
    }
    auto bind = make_uniq<DecodeBindData>();
    auto describe = ParseDescribe(input.inputs[0].ToString(), "decode");
    bind->fields = std::move(describe.fields);
    bind->records = sfjson::SplitArrayObjects(input.inputs[1].ToString());

    if (bind->fields.empty()) {
        throw BinderException("salesforce_decode: fields_json described no fields");
    }
    for (auto &f : bind->fields) {
        names.push_back(f.name);
        return_types.push_back(f.duckdb_type);
    }
    return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> DecodeInitGlobal(ClientContext &,
                                                             TableFunctionInitInput &) {
    return make_uniq<DecodeGlobalState>();
}

static void DecodeFunction(ClientContext &, TableFunctionInput &data, DataChunk &output) {
    auto &bind = data.bind_data->Cast<DecodeBindData>();
    auto &gstate = data.global_state->Cast<DecodeGlobalState>();

    idx_t row = 0;
    while (row < STANDARD_VECTOR_SIZE && gstate.cursor < bind.records.size()) {
        const string &record = bind.records[gstate.cursor];
        for (idx_t col = 0; col < bind.fields.size(); col++) {
            AppendJsonValue(output.data[col], row, bind.fields[col], record);
        }
        gstate.cursor++;
        row++;
    }
    output.SetCardinality(row);
}

} // namespace

TableFunction GetSalesforceDecodeFunction() {
    return TableFunction("salesforce_decode",
                         {LogicalType::VARCHAR, LogicalType::VARCHAR}, DecodeFunction,
                         DecodeBind, DecodeInitGlobal);
}

} // namespace duckdb

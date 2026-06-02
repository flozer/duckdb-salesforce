#pragma once

#include "duckdb.hpp"

// Minimal JSON readers — just enough for Salesforce token + describe responses
// (flat objects of scalar values, plus the describe "fields" array of flat
// objects). Intentionally tiny; a full JSON parser can replace this later
// without changing call sites. Never throws.

namespace duckdb {

namespace sfjson {

// Skip whitespace.
inline void SkipWs(const string &s, size_t &i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        i++;
    }
}

// Position just after `"key"` <ws> ':' <ws>. Returns npos if not found.
inline size_t FindValue(const string &json, const string &key) {
    const string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == string::npos) {
        return string::npos;
    }
    size_t i = k + needle.size();
    SkipWs(json, i);
    if (i >= json.size() || json[i] != ':') {
        return string::npos;
    }
    i++;
    SkipWs(json, i);
    return i;
}

// Parse a JSON string whose opening quote is at json[pos]. Returns the
// unescaped content; advances nothing (pos is the quote index).
inline string ReadStringAt(const string &json, size_t pos) {
    string out;
    size_t i = pos + 1; // past opening quote
    while (i < json.size()) {
        char c = json[i++];
        if (c == '\\' && i < json.size()) {
            char e = json[i++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '/': out.push_back('/'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: out.push_back(e); break;
            }
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Top-level string value for `key`. "" if absent or not a string.
inline string GetString(const string &json, const string &key) {
    size_t i = FindValue(json, key);
    if (i == string::npos || i >= json.size() || json[i] != '"') {
        return "";
    }
    return ReadStringAt(json, i);
}

// Read the raw value for `key`. found=false if the key is absent. is_null=true
// if the value is JSON null. Otherwise `out` holds the unescaped string content
// (for string values) or the literal token (numbers/booleans). Compound
// values (objects/arrays) are returned as their raw substring is not parsed —
// out is left empty.
inline void GetValue(const string &json, const string &key, string &out, bool &found,
                     bool &is_null) {
    out.clear();
    found = false;
    is_null = false;
    size_t i = FindValue(json, key);
    if (i == string::npos || i >= json.size()) {
        return;
    }
    found = true;
    if (json.compare(i, 4, "null") == 0) {
        is_null = true;
        return;
    }
    if (json[i] == '"') {
        out = ReadStringAt(json, i);
        return;
    }
    // Literal token (number / true / false): read until a value terminator.
    size_t j = i;
    while (j < json.size() && json[j] != ',' && json[j] != '}' && json[j] != ']' &&
           json[j] != ' ' && json[j] != '\t' && json[j] != '\n' && json[j] != '\r') {
        j++;
    }
    out = json.substr(i, j - i);
}

// Bool value for `key`. Returns `dflt` if absent/null/not a bool.
inline bool GetBool(const string &json, const string &key, bool dflt) {
    size_t i = FindValue(json, key);
    if (i == string::npos) {
        return dflt;
    }
    if (json.compare(i, 4, "true") == 0) {
        return true;
    }
    if (json.compare(i, 5, "false") == 0) {
        return false;
    }
    return dflt;
}

// Integer value for `key`. Returns `dflt` if absent/null/not numeric.
inline int64_t GetInt(const string &json, const string &key, int64_t dflt) {
    size_t i = FindValue(json, key);
    if (i == string::npos || i >= json.size()) {
        return dflt;
    }
    size_t start = i;
    if (json[i] == '-' || json[i] == '+') {
        i++;
    }
    bool any = false;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        i++;
        any = true;
    }
    if (!any) {
        return dflt;
    }
    try {
        return std::stoll(json.substr(start, i - start));
    } catch (...) {
        return dflt;
    }
}

// Return the balanced "{...}" object that is the value of `key`, or "" if `key`
// is absent or its value is not an object. (Used for nested relationship +
// /limits sub-objects.)
inline string ExtractObject(const string &json, const string &key) {
    size_t i = FindValue(json, key);
    if (i == string::npos || i >= json.size() || json[i] != '{') {
        return "";
    }
    size_t start = i;
    int depth = 0;
    bool in_str = false;
    for (; i < json.size(); i++) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            if (--depth == 0) {
                return json.substr(start, i - start + 1);
            }
        }
    }
    return "";
}

// Read a JSON array of strings for `key` (e.g. describe "referenceTo").
inline vector<string> GetStringArray(const string &json, const string &key) {
    vector<string> out;
    size_t v = FindValue(json, key);
    if (v == string::npos || v >= json.size() || json[v] != '[') {
        return out;
    }
    size_t i = v + 1;
    for (; i < json.size(); i++) {
        char c = json[i];
        if (c == ']') {
            break;
        }
        if (c == '"') {
            out.push_back(ReadStringAt(json, i));
            // skip to the closing quote of this string
            i++;
            while (i < json.size()) {
                if (json[i] == '\\') {
                    i++;
                } else if (json[i] == '"') {
                    break;
                }
                i++;
            }
        }
    }
    return out;
}

// Return each top-level JSON object substring inside the array value of
// `arrayKey`. String literals are tracked so braces inside strings are ignored.
inline vector<string> GetObjectArray(const string &json, const string &arrayKey) {
    vector<string> out;
    size_t v = FindValue(json, arrayKey);
    if (v == string::npos || v >= json.size() || json[v] != '[') {
        return out;
    }
    size_t i = v + 1;
    int depth = 0;
    bool in_obj = false, in_str = false;
    size_t obj_start = 0;
    for (; i < json.size(); i++) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) {
                in_obj = true;
                obj_start = i;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && in_obj) {
                out.push_back(json.substr(obj_start, i - obj_start + 1));
                in_obj = false;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

// Split each top-level object inside a bare JSON array string ("[ {...}, ... ]").
inline vector<string> SplitArrayObjects(const string &json) {
    vector<string> out;
    size_t i = json.find('[');
    if (i == string::npos) {
        return out;
    }
    i++;
    int depth = 0;
    bool in_obj = false, in_str = false;
    size_t obj_start = 0;
    for (; i < json.size(); i++) {
        char c = json[i];
        if (in_str) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            if (depth == 0) {
                in_obj = true;
                obj_start = i;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && in_obj) {
                out.push_back(json.substr(obj_start, i - obj_start + 1));
                in_obj = false;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return out;
}

} // namespace sfjson

} // namespace duckdb

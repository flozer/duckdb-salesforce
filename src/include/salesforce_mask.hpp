#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Replace each non-empty secret value with a fixed redaction marker. Apply to
// any string before it could reach a log/trace/error sink. Required by
// SECURITY.md for Authorization headers, access_token, refresh_token and
// client_secret.
inline string RedactSecrets(string text, const vector<string> &secrets) {
    static const string kMask = "***REDACTED***";
    for (const auto &s : secrets) {
        if (s.empty()) {
            continue;
        }
        size_t pos = 0;
        while ((pos = text.find(s, pos)) != string::npos) {
            text.replace(pos, s.size(), kMask);
            pos += kMask.size();
        }
    }
    return text;
}

} // namespace duckdb

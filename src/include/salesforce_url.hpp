#pragma once

#include "duckdb.hpp"

#include <cctype>

namespace duckdb {

// Percent-encode a URL component (RFC 3986 unreserved kept verbatim; everything
// else %XX, including space -> %20). Used for the SOQL q= parameter and exposed
// as the sf_url_encode() scalar for testing.
inline string UrlEncodeComponent(const string &s) {
    static const char *hex = "0123456789ABCDEF";
    string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

} // namespace duckdb

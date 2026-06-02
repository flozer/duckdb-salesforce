#pragma once

#include "duckdb.hpp"

namespace duckdb {

namespace sfcsv {

// Minimal RFC 4180 CSV parser for Salesforce Bulk API 2.0 results. Handles:
// quoted fields, "" escapes, commas/newlines inside quotes, LF or CRLF row
// endings. An unquoted empty field is an empty string (Bulk represents NULL as
// an empty field; the decoder treats empty as NULL). Returns rows of cells.
inline vector<vector<string>> Parse(const string &data) {
    vector<vector<string>> rows;
    vector<string> row;
    string cell;
    bool in_quotes = false;
    bool field_started = false; // any char seen for the current record
    size_t i = 0;
    auto end_field = [&]() {
        row.push_back(cell);
        cell.clear();
    };
    auto end_row = [&]() {
        end_field();
        rows.push_back(row);
        row.clear();
        field_started = false;
    };
    while (i < data.size()) {
        char c = data[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < data.size() && data[i + 1] == '"') {
                    cell.push_back('"');
                    i += 2;
                    continue;
                }
                in_quotes = false;
                i++;
                continue;
            }
            cell.push_back(c);
            i++;
            continue;
        }
        if (c == '"') {
            in_quotes = true;
            field_started = true;
            i++;
            continue;
        }
        if (c == ',') {
            end_field();
            field_started = true;
            i++;
            continue;
        }
        if (c == '\r') {
            i++; // handled by the following \n (or treat lone CR as row end)
            if (i < data.size() && data[i] == '\n') {
                i++;
            }
            end_row();
            continue;
        }
        if (c == '\n') {
            end_row();
            i++;
            continue;
        }
        cell.push_back(c);
        field_started = true;
        i++;
    }
    // Flush a trailing record (no terminating newline).
    if (field_started || !cell.empty() || !row.empty()) {
        end_row();
    }
    return rows;
}

} // namespace sfcsv

} // namespace duckdb

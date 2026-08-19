#include "sheet_pivot.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

#include "sheet_layout.h"

namespace lugbulk {

namespace {

std::string cell(const std::vector<std::string>& row, size_t idx) {
    if (idx >= row.size()) return "";
    return row[idx];
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Finds every person's qty column by scanning the subheader row for cells
// reading "qty" — this has held across every sheet layout seen (2023-2026)
// even though the *position* of the first person column has not (an extra
// "Total QTY" column in older sheets shifted every person's column right
// by one). Scanning is far more robust to future layout drift than any
// fixed column range would be.
std::vector<size_t> find_person_qty_columns(const std::vector<std::string>& subheader) {
    std::vector<size_t> cols;
    for (size_t i = 0; i < subheader.size(); ++i) {
        if (to_lower(trim(subheader[i])) == layout::kQtyMarker) {
            cols.push_back(i);
        }
    }
    return cols;
}

}  // namespace

double parse_qty(const std::string& qty) {
    std::string stripped;
    stripped.reserve(qty.size());
    for (char c : qty) {
        if (c != ',') stripped.push_back(c);
    }
    stripped = trim(stripped);
    if (stripped.empty()) throw std::invalid_argument("empty qty");

    size_t consumed = 0;
    double value;
    try {
        value = std::stod(stripped, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("non-numeric qty: " + qty);
    }
    if (consumed != stripped.size()) {
        throw std::invalid_argument("non-numeric qty: " + qty);
    }
    return value;
}

PivotResult pivot_sheet(const std::vector<std::vector<std::string>>& rows) {
    PivotResult result;
    if (static_cast<int>(rows.size()) <= layout::kDataStartRow) {
        return result;
    }

    const std::vector<std::string>& header = rows[layout::kHeaderRow];
    const std::vector<std::string>& subheader = rows[layout::kSubheaderRow];
    std::vector<size_t> qty_cols = find_person_qty_columns(subheader);
    std::set<std::pair<std::string, std::string>> seen;  // (person, element_id)

    for (size_t offset = layout::kDataStartRow; offset < rows.size(); ++offset) {
        int sheet_row = static_cast<int>(offset) + 1;  // 1-indexed, matches Sheets UI
        const std::vector<std::string>& row = rows[offset];
        if (row.empty()) continue;

        std::string element_id = cell(row, layout::kColElementId);
        if (element_id.empty()) continue;  // blank/footer row

        std::string description = cell(row, layout::kColDescription);
        if (description.empty()) {
            result.issues.push_back(
                {sheet_row, "missing_description", "Element " + element_id + " has no description"});
        }

        std::string color = cell(row, layout::kColColor);
        if (color.empty()) {
            result.issues.push_back(
                {sheet_row, "missing_color", "Element " + element_id + " has no color"});
        }

        std::string image_url = layout::image_url_for(element_id);

        for (size_t qty_col : qty_cols) {
            std::string person = cell(header, qty_col);
            if (person.empty()) continue;

            std::string qty = trim(cell(row, qty_col));
            if (qty.empty()) continue;  // blank cell, not a mistake

            double qty_num;
            try {
                qty_num = parse_qty(qty);
            } catch (const std::invalid_argument&) {
                result.issues.push_back({sheet_row, "bad_qty",
                                          person + "'s qty for element " + element_id +
                                              " is non-numeric: '" + qty + "'"});
                continue;
            }
            if (qty_num <= 0) continue;

            auto key = std::make_pair(person, element_id);
            if (seen.count(key)) {
                result.issues.push_back(
                    {sheet_row, "duplicate",
                     person + " has more than one qty entry for element " + element_id});
            }
            seen.insert(key);

            result.records.push_back(
                LabelRecord{person, element_id, description, color, qty, image_url});
        }
    }

    return result;
}

}  // namespace lugbulk

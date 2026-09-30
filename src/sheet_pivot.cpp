#include "sheet_pivot.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>
#include <utility>

#include "colors.h"
#include "sheet_layout.h"

namespace lugbulk {

namespace {

using Row = std::vector<std::string>;

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string cell(const Row& row, std::optional<size_t> idx) {
    if (!idx || *idx >= row.size()) return "";
    return trim(row[*idx]);
}

// "120.50", "$120.50", "€1,234" — a person's running cost header cell.
bool is_number(const std::string& text) {
    static const std::regex kNumber(R"(^(\$|€|£)?\s*-?[\d,]*\.?\d+$)");
    return std::regex_match(trim(text), kNumber);
}

// First header matching a candidate, in candidate priority order. If more
// than one matches, prefer one whose column actually has data — an export
// has been seen with a blank "BL Color" beside a populated "LEGO Color".
template <size_t N>
std::optional<size_t> find_col(const Row& header,
                               const std::array<std::string_view, N>& candidates,
                               const std::vector<Row>& rows = {}, size_t data_start = 0) {
    std::vector<size_t> matches;
    for (std::string_view name : candidates) {
        std::string want = to_lower(std::string(name));
        for (size_t col = 0; col < header.size(); ++col) {
            if (to_lower(trim(header[col])) == want) {
                matches.push_back(col);
                break;
            }
        }
    }
    for (size_t col : matches) {
        for (size_t r = data_start; r < rows.size() && r < data_start + 50; ++r) {
            if (!cell(rows[r], col).empty()) return col;
        }
    }
    if (matches.empty()) return std::nullopt;
    return matches.front();
}

std::optional<size_t> find_header_row(const std::vector<Row>& rows) {
    std::vector<size_t> order{static_cast<size_t>(layout::kHeaderRow)};
    for (size_t r = 0; r < static_cast<size_t>(layout::kHeaderSearchRows); ++r) order.push_back(r);
    for (size_t r : order) {
        if (r < rows.size() && find_col(rows[r], layout::kElementIdHeaders)) return r;
    }
    return std::nullopt;
}

using People = std::vector<std::pair<size_t, std::string>>;  // (qty column, name)

People qty_marker_columns(const Row& header, const Row& subheader) {
    People people;
    for (size_t col = 0; col < subheader.size(); ++col) {
        if (to_lower(trim(subheader[col])) == layout::kQtyMarker) {
            std::string name = cell(header, col);
            if (!name.empty()) people.emplace_back(col, name);
        }
    }
    return people;
}

// A contiguous run of (name, cost) header pairs. Single metadata headers
// ("BL Price", "Nominated for", ...) aren't followed by a number so they're
// skipped; stop at the first break in the run — exports have unrelated
// debris further right.
People name_cost_pair_columns(const Row& header, size_t scan_start) {
    auto is_pair = [&](size_t col) {
        if (col + 1 >= header.size()) return false;
        std::string name = cell(header, col);
        return !name.empty() && !is_number(name) && is_number(cell(header, col + 1));
    };
    size_t col = scan_start;
    while (col < header.size() && !is_pair(col)) ++col;
    People people;
    for (; col < header.size() && is_pair(col); col += 2) people.emplace_back(col, cell(header, col));
    return people;
}

std::string format_qty(double qty) {
    if (qty == static_cast<double>(static_cast<long long>(qty))) {
        return std::to_string(static_cast<long long>(qty));
    }
    std::string s = std::to_string(qty);
    s.erase(s.find_last_not_of('0') + 1);
    return s;
}

// What sheets put in a color cell when they don't know it; treated as blank.
std::string unless_placeholder(const std::string& color) {
    static const std::set<std::string> kPlaceholders{"unknown", "n/a", "na", "?", "-", "tbd", "none"};
    return kPlaceholders.count(to_lower(color)) ? "" : color;
}

std::optional<double> parse_weight(const std::string& raw, const std::string& element_id,
                                   int sheet_row, std::vector<SheetIssue>& issues) {
    std::string text = to_lower(trim(raw));
    if (!text.empty() && text.back() == 'g') text = trim(text.substr(0, text.size() - 1));
    if (text.empty()) return std::nullopt;
    try {
        double weight = parse_qty(text);
        if (weight > 0) return weight;
        return std::nullopt;
    } catch (const std::invalid_argument&) {
        issues.push_back({sheet_row, "bad_weight",
                          "Element " + element_id + " has a non-numeric weight: '" + raw + "'"});
        return std::nullopt;
    }
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

bool is_valid_element_id(const std::string& element_id) {
    if (element_id.size() < 4 || element_id.size() > 8) return false;
    return std::all_of(element_id.begin(), element_id.end(),
                       [](unsigned char c) { return std::isdigit(c); });
}

PivotResult pivot_sheet(const std::vector<std::vector<std::string>>& rows) {
    PivotResult result;
    if (rows.empty()) return result;

    std::optional<size_t> found_header = find_header_row(rows);
    size_t header_row = found_header.value_or(layout::kHeaderRow);
    if (header_row >= rows.size()) return result;
    const Row& header = rows[header_row];
    const Row empty_row;
    const Row& subheader = header_row + 1 < rows.size() ? rows[header_row + 1] : empty_row;
    size_t data_start = header_row + 2;

    std::optional<size_t> col_id, col_desc, col_lego, col_bl, col_weight;
    if (!found_header) {
        col_id = layout::kColElementId;
        col_desc = layout::kColDescription;
        col_bl = layout::kColColor;
    } else {
        col_id = find_col(header, layout::kElementIdHeaders);
        col_desc = find_col(header, layout::kDescriptionHeaders, rows, data_start);
        col_lego = find_col(header, layout::kLegoColorHeaders, rows, data_start);
        col_bl = find_col(header, layout::kBlColorHeaders, rows, data_start);
        col_weight = find_col(header, layout::kWeightHeaders, rows, data_start);
        if (!col_desc) col_desc = layout::kColDescription;
    }

    People people = qty_marker_columns(header, subheader);
    if (people.empty()) {
        size_t last_front = 0;
        for (auto c : {col_id, col_desc, col_lego, col_bl, col_weight}) {
            if (c) last_front = std::max(last_front, *c);
        }
        people = name_cost_pair_columns(header, last_front + 1);
    }

    std::set<std::pair<std::string, std::string>> seen;  // (person, element_id)

    for (size_t offset = data_start; offset < rows.size(); ++offset) {
        int sheet_row = static_cast<int>(offset) + 1;  // 1-indexed, matches Sheets UI
        const Row& row = rows[offset];

        std::string element_id = cell(row, col_id);
        if (element_id.empty()) continue;  // blank/footer row
        if (!is_valid_element_id(element_id)) {
            bool has_qty = std::any_of(people.begin(), people.end(),
                                       [&](const auto& p) { return !cell(row, p.first).empty(); });
            if (has_qty) {
                result.issues.push_back({sheet_row, "bad_element_id",
                                         "'" + element_id +
                                             "' isn't a LEGO element ID; row skipped"});
            }
            continue;
        }

        std::string description = cell(row, col_desc);
        if (description.empty()) {
            result.issues.push_back(
                {sheet_row, "missing_description", "Element " + element_id + " has no description"});
        }

        std::vector<std::pair<std::string, std::string>> wanted;  // (person, qty)
        for (const auto& [qty_col, person] : people) {
            std::string qty = cell(row, qty_col);
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
            if (qty_num > 0) wanted.emplace_back(person, format_qty(qty_num));
        }
        if (wanted.empty()) continue;  // nobody ordered it; don't nag about its colors

        std::string lego = unless_placeholder(cell(row, col_lego));
        std::string bl = unless_placeholder(cell(row, col_bl));
        if (lego.empty() && bl.empty()) {
            result.issues.push_back(
                {sheet_row, "missing_color", "Element " + element_id + " has no color", element_id});
        } else {
            colors::Resolved c = colors::resolve(lego, bl);
            if (!c.mapped) {
                std::string known = c.lego.empty() ? c.bl : c.lego;
                result.issues.push_back(
                    {sheet_row, "unmapped_color",
                     "Element " + element_id + ": don't know the " +
                         (c.lego.empty() ? "LEGO" : "BrickLink") + " name for '" + known +
                         "' — fill in the sheet's " +
                         (c.lego.empty() ? "LEGO Color" : "BL Color") + " column"});
            }
            lego = c.lego;
            bl = c.bl;
        }

        std::optional<double> weight =
            parse_weight(cell(row, col_weight), element_id, sheet_row, result.issues);
        std::string image_url = layout::image_url_for(element_id);

        for (auto& [person, qty] : wanted) {
            if (!seen.insert({person, element_id}).second) {
                result.issues.push_back(
                    {sheet_row, "duplicate",
                     person + " has more than one qty entry for element " + element_id});
            }
            LabelRecord rec;
            rec.person = person;
            rec.element_id = element_id;
            rec.description = description;
            rec.lego_color = lego;
            rec.bl_color = bl;
            rec.qty = qty;
            rec.image_url = image_url;
            rec.weight = weight;
            result.records.push_back(std::move(rec));
        }
    }

    return result;
}

}  // namespace lugbulk

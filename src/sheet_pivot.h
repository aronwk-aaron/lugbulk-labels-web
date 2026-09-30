// Pivots the wide per-person qty matrix (the "Order Here" tab) into one
// label record per (person, part) pair where qty > 0. Port of
// lugbulk-label's pivot.py — see that file for the reference behavior
// this mirrors. Two sheet layouts are handled:
//
//   - "qty marker": person names on the header row, and the row below
//     marks each person's qty column "qty" (paired with a "$$" column).
//   - "name/cost pair": each person is a header cell holding their name
//     followed by one holding their running cost total (2026's master
//     sheet), with a totals row above the header.
//
// Front-matter columns (element id, description, LEGO/BL color, weight)
// are found by header text, falling back to fixed positions.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace lugbulk {

struct LabelRecord {
    std::string person;
    std::string element_id;  // digits only — validated, safe to use in paths/URLs
    std::string description;
    std::string lego_color;
    std::string bl_color;
    std::string qty;  // normalized display text, e.g. "2000"
    std::string image_url;
    std::optional<double> weight;  // grams per piece, from the sheet's Weight column
    std::optional<double> catalog_weight;  // grams per piece, from BrickLink (bricklink.h)
    // Filled in by ordering::order_records: "part_seq of part_total".
    int part_seq = 0;
    int part_total = 0;
};

struct SheetIssue {
    int row;  // 1-indexed sheet row, matches the Sheets UI
    // "duplicate" | "bad_qty" | "bad_element_id" | "missing_description" |
    // "missing_color" | "unmapped_color" | "bad_weight"
    std::string kind;
    std::string detail;
    std::string element_id = {};  // set for "missing_color", so a later lookup can clear it
};

// Pivots raw Sheets API rows (as returned by oauth::fetch_sheet_values) into
// label records, collecting SheetIssues for anything that looks like a
// data-entry mistake along the way (never throws on bad data — issues are
// how bad data surfaces to the caller).
struct PivotResult {
    std::vector<LabelRecord> records;
    std::vector<SheetIssue> issues;
};

PivotResult pivot_sheet(const std::vector<std::vector<std::string>>& rows);

// Parses a Sheets-formatted quantity string ("2,000", "150", etc.) into a
// double, stripping thousands separators. Throws std::invalid_argument if
// it's not numeric after stripping. A LabelRecord's own qty always parses.
double parse_qty(const std::string& qty);

// LEGO element IDs are 4-8 digits. Anything else is a typo or a stray
// notes/footer row — and since the ID becomes an image cache filename and
// part of the image URL, it must never contain path characters.
bool is_valid_element_id(const std::string& element_id);

}  // namespace lugbulk

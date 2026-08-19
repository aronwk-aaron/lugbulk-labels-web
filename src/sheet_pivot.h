// Pivots the wide per-person qty matrix (the "Order Here" tab) into one
// label record per (person, part) pair where qty > 0. Port of
// lugbulk-label's sheets_source.py — see that file for the reference
// behavior this mirrors.
//
// Person columns are discovered by scanning the subheader row for "qty"
// markers (see sheet_layout.h's kQtyMarker) rather than assumed at a fixed
// column range — verified against real sheets from 2023-2026, where the
// exact column an event's roster starts at has shifted between years (an
// extra "Total QTY" column in older sheets pushed everyone right by one).
#pragma once

#include <string>
#include <vector>

namespace lugbulk {

struct LabelRecord {
    std::string person;
    std::string element_id;
    std::string description;
    std::string color;
    std::string qty;        // display text as it appeared on the sheet, e.g. "2,000"
    std::string image_url;
};

struct SheetIssue {
    int row;              // 1-indexed sheet row, matches the Sheets UI
    std::string kind;      // "duplicate" | "bad_qty" | "missing_description" | "missing_color"
    std::string detail;
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
// it's not numeric after stripping — callers that already ran pivot_sheet
// only see qty strings that parsed cleanly (bad ones became a "bad_qty"
// SheetIssue and were excluded from records), so this should never throw
// on a LabelRecord's own qty field.
double parse_qty(const std::string& qty);

}  // namespace lugbulk

// "Order Here" tab layout constants — mirrors lugbulk-label (the CLI
// counterpart)'s config.py. Fixed template, not per-sheet configurable in
// v1 (see README's "Out of scope for v1").
#pragma once

#include <cstddef>
#include <string>

namespace lugbulk::layout {

inline constexpr const char* kSourceTab = "Order Here";

// 0-indexed columns within the tab. Only these three "front matter" columns
// are assumed fixed — they've been stable across every sheet year we've
// seen (2023-2026). Where each person's qty column actually starts, and how
// many there are, is NOT fixed (it has shifted between years, e.g. an
// extra "Total QTY" column pushed 2023/2024's first person from col 7 to
// col 8) — see kSubheaderRow below for how those are found instead.
inline constexpr int kColElementId = 1;
inline constexpr int kColDescription = 3;
inline constexpr int kColColor = 4;
inline constexpr int kHeaderRow = 0;    // person names live here
inline constexpr int kSubheaderRow = 1; // "qty" / "$$" markers live here — see kQtyMarker
inline constexpr int kDataStartRow = 2; // first row of actual part data

// A person's qty column is identified by its row-1 (kSubheaderRow) cell
// reading "qty" (case-insensitive) — this has been the one reliable marker
// across every sheet layout seen so far, unlike raw column position. Each
// such column is paired with the $-cost column immediately after it (not
// itself scanned for records, but skipped as part of the same person).
inline constexpr const char* kQtyMarker = "qty";

// LEGO element photo CDN — built from Element ID, since the sheet's own
// Photo column is an in-cell =IMAGE() formula the Sheets API can't return.
inline std::string image_url_for(const std::string& element_id) {
    return "https://www.lego.com/cdn/product-assets/element.img.lod5photo.192x192/" +
           element_id + ".jpg";
}

// Avery 5160: 1" x 2-5/8", 3 across x 10 down, 30/sheet. The only label
// spec carried over to v1 (README: "--label-spec format choice" dropped).
struct LabelSpec {
    double sheet_width_mm = 215.9, sheet_height_mm = 279.4;  // US Letter
    int columns = 3, rows = 10;
    double label_width_mm = 66.675, label_height_mm = 25.4;
    double left_margin_mm = 4.7625, right_margin_mm = 4.7625;
    double top_margin_mm = 12.7, bottom_margin_mm = 12.7;
    double row_gap_mm = 0, column_gap_mm = 3.175;

    int per_sheet() const { return columns * rows; }
};

inline constexpr LabelSpec kAvery5160{};

}  // namespace lugbulk::layout

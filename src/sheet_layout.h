// "Order Here" tab layout constants and label formats — mirrors
// lugbulk-label (the CLI counterpart)'s config.py.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lugbulk::layout {

inline constexpr const char* kSourceTab = "Order Here";

// Front-matter columns are found by header text (first match wins, in
// priority order). The 0-indexed positions below are only the fallback
// for a sheet with no recognizable header row at all.
inline constexpr std::array<std::string_view, 2> kElementIdHeaders{"Element ID", "Part Number"};
inline constexpr std::array<std::string_view, 1> kDescriptionHeaders{"Description"};
inline constexpr std::array<std::string_view, 2> kLegoColorHeaders{"LEGO Color", "LEGO Colour"};
inline constexpr std::array<std::string_view, 4> kBlColorHeaders{"BL Color", "BrickLink Color",
                                                                  "BL Colour", "Color"};
inline constexpr std::array<std::string_view, 3> kWeightHeaders{"Weight", "Weight (g)",
                                                                 "Weight g"};

inline constexpr int kColElementId = 1;
inline constexpr int kColDescription = 3;
inline constexpr int kColColor = 4;  // "BL Color"
inline constexpr int kHeaderRow = 0;  // person names live here
// Exports have been seen with extra rows (e.g. totals) above the real
// header, so the first this-many rows are searched for it.
inline constexpr int kHeaderSearchRows = 10;

// In the "qty marker" layout, a person's qty column is identified by the
// cell below its header reading "qty" (case-insensitive) — the one
// reliable marker across sheet years, unlike raw column position (an extra
// "Total QTY" column in 2023/2024 pushed everyone right by one). Sheets
// without markers use (name, running cost) header pairs instead — see
// sheet_pivot.cpp.
inline constexpr const char* kQtyMarker = "qty";

// LEGO element photo CDN — built from Element ID, since the sheet's own
// Photo column is an in-cell =IMAGE() formula the Sheets API can't return.
inline std::string image_url_for(const std::string& element_id) {
    return "https://www.lego.com/cdn/product-assets/element.img.lod5photo.192x192/" +
           element_id + ".jpg";
}

// A label stock. Sheet stock is laid out as a grid on its page; a roll
// label (Dymo) is one label per page, page size = label size (stored
// landscape). The inventory — Avery US-Letter and A4, Dymo LabelWriter —
// lives in data/label_specs.json, generated from the gLabels template
// database by lugbulk-label's tools/update_label_specs.py.
struct LabelSpec {
    std::string id;  // e.g. "avery5162", "dymo30857"
    std::string brand, part, description;
    std::string page;  // "US-Letter" | "A4" | "roll"
    std::vector<std::string> equivalents;  // other part numbers for the same stock
    double sheet_width_mm, sheet_height_mm;
    int columns, rows;
    double label_width_mm, label_height_mm;
    double left_margin_mm, top_margin_mm;
    double row_gap_mm, column_gap_mm;

    int per_sheet() const { return columns * rows; }
    // Human-readable, e.g. `Avery 5162 — Address labels, 1.33" x 4.00", 14/sheet`.
    std::string display_name() const;
};

inline constexpr const char* kDefaultLabelSpecId = "avery5162";

// Loads the inventory; call once at startup. Throws std::runtime_error if
// the file is missing or malformed, or lacks the default stock.
void load_label_specs(const std::string& path);

// The loaded inventory, in display order (brand, page size, part number).
const std::vector<LabelSpec>& label_specs();

// Looks a stock up by id, "Avery 8162", an equivalent part number, or a
// bare part number ("5162"). nullptr if unknown.
const LabelSpec* find_label_spec(std::string_view name);

const LabelSpec& default_label_spec();

}  // namespace lugbulk::layout

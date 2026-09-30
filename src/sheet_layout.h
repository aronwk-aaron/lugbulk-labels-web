// "Order Here" tab layout constants and label formats — mirrors
// lugbulk-label (the CLI counterpart)'s config.py.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

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

// A label format. Sheet stock is laid out as a grid on US Letter; a roll
// label (Dymo) is one label per page, page size = label size.
struct LabelSpec {
    const char* id;
    const char* name;  // shown in the dashboard's picker
    double sheet_width_mm, sheet_height_mm;
    int columns, rows;
    double label_width_mm, label_height_mm;
    double left_margin_mm, top_margin_mm;
    double row_gap_mm, column_gap_mm;

    int per_sheet() const { return columns * rows; }
};

inline constexpr std::array<LabelSpec, 4> kLabelSpecs{{
    // 1-1/3" x 4", 2 across x 7 down, 14/sheet
    {"avery5162", "Avery 5162/8162 (1-1/3\" x 4\", 14/sheet)", 215.9, 279.4, 2, 7, 101.6,
     33.867, 3.969, 21.167, 0, 4.763},
    // Dymo LabelWriter 30857 name badge, 2-1/4" x 4", one per page
    {"dymo30857", "Dymo 30857 (2-1/4\" x 4\", roll)", 101.6, 57.15, 1, 1, 101.6, 57.15, 0, 0,
     0, 0},
    // 2" x 4", 2 across x 5 down, 10/sheet
    {"avery5163", "Avery 5163 (2\" x 4\", 10/sheet)", 215.9, 279.4, 2, 5, 101.6, 50.8, 3.9,
     12.7, 0, 4.9},
    // 1" x 2-5/8", 3 across x 10 down, 30/sheet — the 2026 size; cramped
    {"avery5160", "Avery 5160 (1\" x 2-5/8\", 30/sheet)", 215.9, 279.4, 3, 10, 66.675, 25.4,
     4.7625, 12.7, 0, 3.175},
}};

inline constexpr const LabelSpec& kDefaultLabelSpec = kLabelSpecs[0];

// Looks a spec up by id; nullptr if unknown.
inline const LabelSpec* find_label_spec(std::string_view id) {
    for (const auto& spec : kLabelSpecs) {
        if (id == spec.id) return &spec;
    }
    return nullptr;
}

}  // namespace lugbulk::layout

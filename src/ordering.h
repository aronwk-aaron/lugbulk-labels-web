// Part sizing, label ordering, and per-part summaries — port of
// lugbulk-label's ordering.py.
//
// Labels come out grouped by part: parts ordered by (estimated) weight,
// heaviest first by default, and within a part by quantity, smallest first.
// Each label is numbered within its part ("3 of 10") so a sorter can tell
// at a glance when a part's pile is complete.
//
// Weight comes from the sheet's "Weight" column (grams per piece) when
// present, else the BrickLink catalog (bricklink.h, if configured), else an
// estimate from the description's stud dimensions
// ("PLATE 4X8", "BRICK 1X2X5", "BRICK 1X1X1 2/3"). Parts neither covers
// (plants, animals, minifig parts...) sort after everything else, in sheet
// order, and show as "size unknown" in the parts report.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sheet_pivot.h"

namespace lugbulk::ordering {

enum class PartOrder { kHeaviest, kLightest, kSheet };

// Parses "heaviest" / "lightest" / "sheet"; nullopt for anything else.
std::optional<PartOrder> parse_part_order(const std::string& s);

// Grams per piece estimated from a description's stud dimensions, or
// nullopt if it has none.
std::optional<double> estimate_weight(const std::string& description);

struct PartSummary {
    std::string element_id;
    std::string description;
    std::string lego_color;
    std::string bl_color;
    int lots = 0;  // number of people (labels) ordering this part
    double pieces = 0;
    std::optional<double> weight;  // grams per piece
    std::string weight_source;     // "sheet" | "bricklink" | "estimate" | ""
};

// One PartSummary per distinct element, in label order.
std::vector<PartSummary> summarize_parts(const std::vector<LabelRecord>& records,
                                         PartOrder order);

// Records grouped by part in `order`, smallest qty first within a part
// (ties by last name), with part_seq/part_total filled in.
std::vector<LabelRecord> order_records(std::vector<LabelRecord> records, PartOrder order);

}  // namespace lugbulk::ordering

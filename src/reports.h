// Report generation (CSV + PDF) — port of lugbulk-label's manifest.py.
//   - Lot counts: per person, "lot count" = number of (person, part) label
//     lines they have; "pieces" = sum of their qtys.
//   - Parts list: per part, total pieces and how many people ordered it,
//     in label order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ordering.h"
#include "sheet_pivot.h"

namespace lugbulk::reports {

enum class SortBy { kLastName, kFirstName };

// Sort key for a "First Last" name: by default sorts on the last
// whitespace-separated token (falls back to the full name for anything
// that isn't First-Last, e.g. a single-word entry); kFirstName sorts by
// the name as written. Either way, ties break on the full (lowercased) name.
std::pair<std::string, std::string> person_sort_key(const std::string& person, SortBy sort_by);

struct PersonTotals {
    std::string person;
    int lot_count = 0;
    double total_pieces = 0;
};

// One row per person: how many lots (label lines) and total pieces,
// sorted per `sort_by`.
std::vector<PersonTotals> lot_counts_by_person(const std::vector<LabelRecord>& records,
                                                SortBy sort_by);

// RFC 4180 CSV: header "person,lot_count,total_pieces" + one row per person.
std::string lot_counts_csv(const std::vector<LabelRecord>& records, SortBy sort_by);

// One-page table PDF (returned as an in-memory buffer, never written to
// disk): person / lot count / total pieces.
std::vector<uint8_t> lot_counts_pdf(const std::vector<LabelRecord>& records, SortBy sort_by);

// Header "order,element_id,description,lego_color,bl_color,total_pieces,
// people,grams_per_piece,weight_source" + one row per part, in the order given.
std::string parts_csv(const std::vector<ordering::PartSummary>& parts);

// Printable parts list, one row per part, in the order given.
std::vector<uint8_t> parts_pdf(const std::vector<ordering::PartSummary>& parts);

// "~4.6 g/pc" (estimated), "12 g/pc" (from the sheet), or "size unknown".
std::string weight_text(const ordering::PartSummary& part);

}  // namespace lugbulk::reports

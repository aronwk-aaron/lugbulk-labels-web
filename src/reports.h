// Lot-count report generation (CSV + PDF) — port of lugbulk-label's
// manifest.py write_lot_counts_* functions. "Lot count" = number of
// (person, part) label lines a person has; "pieces" = sum of their qtys.
#pragma once

#include <string>
#include <vector>

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

}  // namespace lugbulk::reports

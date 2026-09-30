// What happens to a sheet's rows between reading them and rendering:
// the row cap, the per-run size limits, and BrickLink data — shared by
// Google Sheets reads and uploads. Also the "Check sheet" JSON.
// The browser has a port in static/js/records.js (tests/js/parity.test.mjs
// checks the two agree).
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "bricklink.h"
#include "sheet_pivot.h"

namespace lugbulk::records {

// Size limits on what one request may make the server do.
inline constexpr int kMaxSheetRows = 3000;   // rows read from the "Order Here" tab
inline constexpr size_t kMaxLabels = 20000;  // labels in one run
inline constexpr size_t kMaxParts = 2000;    // distinct parts in one run (photo downloads)

// A sheet over the per-run size limits.
struct TooBig : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Drops rows past kMaxSheetRows.
void cap_rows(std::vector<std::vector<std::string>>& rows);

// Throws TooBig if the pivot is over kMaxLabels labels or kMaxParts parts.
void check_run_size(const PivotResult& pivot);

// Pivots a file's tabs in turn (each capped) until one has orders — a
// workbook's order tab comes first; a CSV is one tab — then checks the
// run size. Throws TooBig.
PivotResult pivot_tabs(std::vector<std::vector<std::vector<std::string>>> tabs);

// Fills in BrickLink data from the catalog: each record's catalog_weight,
// and BrickLink/LEGO color names the sheet left blank (clearing the
// "missing_color" issues that fixes). A no-op for an empty catalog.
void apply_bricklink(PivotResult& pivot, const bricklink::Catalog& catalog);

// The "Check sheet" response:
// {"labels":N,"people":N,"parts":N,"issues":[{"row":N,"kind":"...","detail":"..."}]}.
std::string check_summary_json(const PivotResult& pivot);

// The "sheet check.txt" in the "Download all" zip: "<N> labels", then
// "Row R (kind): detail" per issue, or "No issues found.".
std::string check_text(const PivotResult& pivot);

}  // namespace lugbulk::records

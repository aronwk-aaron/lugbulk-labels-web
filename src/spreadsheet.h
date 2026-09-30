// Reads an uploaded order sheet — an .xlsx workbook or a .csv file (e.g.
// Google Sheets' "Download → Microsoft Excel" or "→ CSV") — into the same
// rows-of-strings shape the Sheets API returns, for pivot_sheet().
//
// Uploads are untrusted, so reading is bounded: the upload size is capped
// by the caller, and an .xlsx (a zip of XML files) may not expand past
// kMaxUnpackedBytes — a zip bomb is rejected, not unpacked.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace lugbulk::spreadsheet {

using Rows = std::vector<std::vector<std::string>>;

inline constexpr size_t kMaxUnpackedBytes = 64 * 1024 * 1024;

// Not a readable spreadsheet (message is safe to show the user).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True if `data` looks like an .xlsx (zip) rather than text.
bool is_xlsx(const std::string& data);

// RFC 4180 CSV: quoted fields, doubled quotes, CR/LF line ends, optional
// UTF-8 BOM.
Rows read_csv(const std::string& data);

// The rows of the workbook's order tab: the sheet named like `tab`
// (ignoring case and spaces, so "OrderHere" matches "Order Here"), or if
// there's none, every sheet in workbook order — the caller tries each.
std::vector<Rows> read_xlsx(const std::string& data, const std::string& tab);

}  // namespace lugbulk::spreadsheet

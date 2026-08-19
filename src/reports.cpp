#include "reports.h"

#include <podofo/podofo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

#include "pdf_text.h"

namespace lugbulk::reports {

namespace {

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Splits on whitespace; used to find the last token ("last name") of a
// "First Last" entry.
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    return parts;
}

// Formats a double the way Python's f"{x:g}" would for the counts here
// (whole numbers print without a trailing ".0"; otherwise a compact
// decimal) — matches manifest.py's total_pieces formatting.
std::string format_g(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        std::ostringstream oss;
        oss << static_cast<long long>(value);
        return oss.str();
    }
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

// Minimal RFC 4180 field quoting: quote if the field contains a comma,
// quote, or newline; double up any embedded quotes.
//
// Also guards against CSV/formula injection (CWE-1236): `person` is the
// sheet's own column header text, editable by anyone with edit access to
// the shared "Order Here" sheet — not just the app user who ends up
// downloading this CSV. A collaborator could set their name to a formula
// (e.g. "=WEBSERVICE(...)" or a DDE payload) that Excel/LibreOffice/Sheets
// would execute when the downloading user opens the file. Per OWASP's CSV
// injection guidance, a field starting with =, +, -, @, tab, or CR is
// prefixed with a leading apostrophe, which spreadsheet apps render as a
// literal quoted-text marker instead of treating the cell as a formula.
std::string csv_field(const std::string& s) {
    std::string field = s;
    if (!field.empty() && field.find_first_of("=+-@\t\r") == 0) {
        field.insert(field.begin(), '\'');
    }

    bool needs_quoting = field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quoting) return field;
    std::string out = "\"";
    for (char c : field) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

}  // namespace

std::pair<std::string, std::string> person_sort_key(const std::string& person, SortBy sort_by) {
    std::vector<std::string> parts = split_ws(person);
    std::string primary;
    if (!parts.empty() && sort_by == SortBy::kLastName) {
        primary = to_lower(parts.back());
    } else {
        primary = to_lower(person);
    }
    return {primary, to_lower(person)};
}

std::vector<PersonTotals> lot_counts_by_person(const std::vector<LabelRecord>& records,
                                                SortBy sort_by) {
    std::map<std::string, PersonTotals> by_person;  // insertion order doesn't matter, we sort after
    for (const auto& r : records) {
        auto& totals = by_person[r.person];
        totals.person = r.person;
        totals.lot_count += 1;
        totals.total_pieces += parse_qty(r.qty);
    }

    std::vector<PersonTotals> out;
    out.reserve(by_person.size());
    for (auto& [_, totals] : by_person) out.push_back(totals);

    std::sort(out.begin(), out.end(), [&](const PersonTotals& a, const PersonTotals& b) {
        return person_sort_key(a.person, sort_by) < person_sort_key(b.person, sort_by);
    });
    return out;
}

std::string lot_counts_csv(const std::vector<LabelRecord>& records, SortBy sort_by) {
    std::vector<PersonTotals> totals = lot_counts_by_person(records, sort_by);
    std::ostringstream out;
    out << "person,lot_count,total_pieces\r\n";
    for (const auto& t : totals) {
        out << csv_field(t.person) << "," << t.lot_count << "," << format_g(t.total_pieces)
            << "\r\n";
    }
    return out.str();
}

std::vector<uint8_t> lot_counts_pdf(const std::vector<LabelRecord>& records, SortBy sort_by) {
    std::vector<PersonTotals> totals = lot_counts_by_person(records, sort_by);

    PoDoFo::PdfRefCountedBuffer buffer;
    PoDoFo::PdfOutputDevice device(&buffer);
    PoDoFo::PdfStreamedDocument doc(&device);

    const double page_w = 612.0, page_h = 792.0;  // US Letter, points (72/in)
    const double margin = 15.0 * (72.0 / 25.4);   // 15mm in points

    PoDoFo::PdfFont* font_bold = doc.CreateFont("Helvetica-Bold");
    PoDoFo::PdfFont* font_regular = doc.CreateFont("Helvetica");
    if (!font_bold || !font_regular) {
        throw std::runtime_error("pdf error: could not load base fonts");
    }

    const double row_h = 16.0;
    const int header_h_rows = 3;  // title + subtitle + spacer, in row units
    const int rows_per_page =
        static_cast<int>((page_h - 2 * margin) / row_h) - header_h_rows - 1 /* table header */;
    int total_pages = totals.empty() ? 1 : static_cast<int>(std::ceil(
                                                static_cast<double>(totals.size()) /
                                                std::max(1, rows_per_page)));

    int total_lots = 0;
    for (const auto& t : totals) total_lots += t.lot_count;

    size_t idx = 0;
    for (int page = 0; page < total_pages; ++page) {
        PoDoFo::PdfPage* pdf_page =
            doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
        PoDoFo::PdfPainter painter;
        painter.SetPage(pdf_page);

        double y = page_h - margin;

        font_bold->SetFontSize(14.0f);
        painter.SetFont(font_bold);
        painter.DrawText(margin, y - 14, PoDoFo::PdfString(pdf_text::to_winansi("Lot counts by person").c_str()));
        y -= row_h;

        font_regular->SetFontSize(9.0f);
        painter.SetFont(font_regular);
        std::string sort_label = (sort_by == SortBy::kLastName) ? "last" : "first";
        std::string subtitle = std::to_string(totals.size()) + " people, " +
                                std::to_string(total_lots) + " lots total \xe2\x80\x94 sorted by " +
                                sort_label + " name";
        painter.DrawText(margin, y - 10, PoDoFo::PdfString(pdf_text::to_winansi(subtitle).c_str()));
        y -= row_h * 2;

        // Table header
        const double col_person_w = 280, col_lots_w = 100, col_pieces_w = 120;
        double x0 = margin;
        painter.SetColor(0.85, 0.85, 0.85);
        painter.Rectangle(x0, y - row_h + 4, col_person_w + col_lots_w + col_pieces_w, row_h);
        painter.Fill();
        painter.SetColor(0, 0, 0);
        font_bold->SetFontSize(9.0f);
        painter.SetFont(font_bold);
        painter.DrawText(x0 + 3, y - row_h + 8, PoDoFo::PdfString("Person"));
        painter.DrawText(x0 + col_person_w + 3, y - row_h + 8, PoDoFo::PdfString("Lots"));
        painter.DrawText(x0 + col_person_w + col_lots_w + 3, y - row_h + 8,
                          PoDoFo::PdfString("Total pieces"));
        y -= row_h;

        font_regular->SetFontSize(9.0f);
        painter.SetFont(font_regular);
        for (int r = 0; r < rows_per_page && idx < totals.size(); ++r, ++idx) {
            const PersonTotals& t = totals[idx];
            if (r % 2 == 1) {
                painter.SetColor(0.96, 0.96, 0.96);
                painter.Rectangle(x0, y - row_h + 4, col_person_w + col_lots_w + col_pieces_w,
                                   row_h);
                painter.Fill();
                painter.SetColor(0, 0, 0);
            }
            painter.DrawText(x0 + 3, y - row_h + 8,
                              PoDoFo::PdfString(pdf_text::to_winansi(t.person).c_str()));
            painter.DrawText(x0 + col_person_w + 3, y - row_h + 8,
                              PoDoFo::PdfString(std::to_string(t.lot_count)));
            painter.DrawText(x0 + col_person_w + col_lots_w + 3, y - row_h + 8,
                              PoDoFo::PdfString(format_g(t.total_pieces)));
            y -= row_h;
        }

        painter.FinishPage();
    }

    doc.Close();

    std::vector<uint8_t> out(buffer.GetSize());
    std::memcpy(out.data(), buffer.GetBuffer(), buffer.GetSize());
    return out;
}

}  // namespace lugbulk::reports

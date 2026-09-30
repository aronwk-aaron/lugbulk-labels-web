#include "reports.h"

#include <podofo/podofo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {

// A simple paginated table PDF: title, subtitle, then a header row and
// zebra-striped body rows, repeated per page. Cell text is shrunk/
// truncated to fit its column.
std::vector<uint8_t> table_pdf(const std::string& title, const std::string& subtitle,
                               const std::vector<std::string>& headers,
                               const std::vector<double>& col_widths,
                               const std::vector<std::vector<std::string>>& rows) {
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

    double table_w = 0;
    for (double w : col_widths) table_w += w;

    // Truncates `text` (UTF-8) with "..." to fit `max_w` at 9pt; returns WinAnsi.
    auto fit = [](PoDoFo::PdfFont* font, const std::string& text, double max_w) {
        std::string t = pdf_text::to_winansi(text);
        font->SetFontSize(9.0f);
        if (font->GetFontMetrics()->StringWidth(t.c_str()) <= max_w) return t;
        while (!t.empty() && font->GetFontMetrics()->StringWidth((t + "...").c_str()) > max_w) {
            t.pop_back();
        }
        return t + "...";
    };

    auto draw_row = [&](PoDoFo::PdfPainter& painter, PoDoFo::PdfFont* font, double y,
                        const std::vector<std::string>& cells) {
        font->SetFontSize(9.0f);
        painter.SetFont(font);
        double x = margin;
        for (size_t c = 0; c < cells.size() && c < col_widths.size(); ++c) {
            std::string text = fit(font, cells[c], col_widths[c] - 6);
            painter.DrawText(x + 3, y - 16 + 8, PoDoFo::PdfString(text.c_str()));
            x += col_widths[c];
        }
    };

    const double row_h = 16.0;
    const int header_h_rows = 3;  // title + subtitle + spacer, in row units
    const int rows_per_page = std::max(
        1, static_cast<int>((page_h - 2 * margin) / row_h) - header_h_rows - 1 /* table header */);
    int total_pages = rows.empty() ? 1 : static_cast<int>(
        (rows.size() + rows_per_page - 1) / rows_per_page);

    size_t idx = 0;
    for (int page = 0; page < total_pages; ++page) {
        PoDoFo::PdfPage* pdf_page = doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
        PoDoFo::PdfPainter painter;
        painter.SetPage(pdf_page);

        double y = page_h - margin;

        font_bold->SetFontSize(14.0f);
        painter.SetFont(font_bold);
        painter.DrawText(margin, y - 14, PoDoFo::PdfString(pdf_text::to_winansi(title).c_str()));
        y -= row_h;

        font_regular->SetFontSize(9.0f);
        painter.SetFont(font_regular);
        painter.DrawText(margin, y - 10, PoDoFo::PdfString(pdf_text::to_winansi(subtitle).c_str()));
        y -= row_h * 2;

        painter.SetColor(0.85, 0.85, 0.85);
        painter.Rectangle(margin, y - row_h + 4, table_w, row_h);
        painter.Fill();
        painter.SetColor(0, 0, 0);
        draw_row(painter, font_bold, y, headers);
        y -= row_h;

        for (int r = 0; r < rows_per_page && idx < rows.size(); ++r, ++idx) {
            if (r % 2 == 1) {
                painter.SetColor(0.96, 0.96, 0.96);
                painter.Rectangle(margin, y - row_h + 4, table_w, row_h);
                painter.Fill();
                painter.SetColor(0, 0, 0);
            }
            draw_row(painter, font_regular, y, rows[idx]);
            y -= row_h;
        }

        painter.FinishPage();
    }

    doc.Close();

    std::vector<uint8_t> out(buffer.GetSize());
    std::memcpy(out.data(), buffer.GetBuffer(), buffer.GetSize());
    return out;
}

std::string join_colors(const ordering::PartSummary& p) {
    if (p.lego_color.empty()) return p.bl_color;
    if (p.bl_color.empty()) return p.lego_color;
    return p.lego_color + " / " + p.bl_color;
}

}  // namespace

std::vector<uint8_t> lot_counts_pdf(const std::vector<LabelRecord>& records, SortBy sort_by) {
    std::vector<PersonTotals> totals = lot_counts_by_person(records, sort_by);
    int total_lots = 0;
    std::vector<std::vector<std::string>> rows;
    for (const auto& t : totals) {
        total_lots += t.lot_count;
        rows.push_back({t.person, std::to_string(t.lot_count), format_g(t.total_pieces)});
    }
    std::string sort_label = (sort_by == SortBy::kLastName) ? "last" : "first";
    std::string subtitle = std::to_string(totals.size()) + " people, " +
                           std::to_string(total_lots) + " lots total \xe2\x80\x94 sorted by " +
                           sort_label + " name";
    return table_pdf("Lot counts by person", subtitle, {"Person", "Lots", "Total pieces"},
                     {280, 100, 120}, rows);
}

std::string weight_text(const ordering::PartSummary& part) {
    if (!part.weight) return "size unknown";
    char buf[32];
    if (*part.weight >= 10) {
        std::snprintf(buf, sizeof(buf), "%.0f", *part.weight);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2g", *part.weight);
    }
    return (part.weight_source == "estimate" ? "~" : "") + std::string(buf) + " g/pc";
}

std::string parts_csv(const std::vector<ordering::PartSummary>& parts) {
    std::ostringstream out;
    out << "order,element_id,description,lego_color,bl_color,total_pieces,people,"
           "grams_per_piece,weight_source\r\n";
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        std::string grams;
        if (p.weight) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3g", *p.weight);
            grams = buf;
        }
        out << (i + 1) << "," << csv_field(p.element_id) << "," << csv_field(p.description) << ","
            << csv_field(p.lego_color) << "," << csv_field(p.bl_color) << ","
            << format_g(p.pieces) << "," << p.lots << "," << grams << "," << p.weight_source
            << "\r\n";
    }
    return out.str();
}

std::vector<uint8_t> parts_pdf(const std::vector<ordering::PartSummary>& parts) {
    double total_pieces = 0;
    int total_labels = 0;
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        total_pieces += p.pieces;
        total_labels += p.lots;
        rows.push_back({std::to_string(i + 1), p.element_id, p.description, join_colors(p),
                        format_g(p.pieces), std::to_string(p.lots), weight_text(p)});
    }
    std::string subtitle = std::to_string(parts.size()) + " parts, " + format_g(total_pieces) +
                           " pieces, " + std::to_string(total_labels) +
                           " labels \xe2\x80\x94 in label order";
    return table_pdf("Parts list", subtitle,
                     {"#", "Element", "Description", "LEGO / BrickLink color", "Pieces", "People",
                      "Weight"},
                     {24, 50, 150, 150, 45, 40, 70}, rows);
}

}  // namespace lugbulk::reports

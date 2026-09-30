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
// zebra-striped body rows, repeated per page. Cell text wraps within
// its column and a row grows to its tallest cell; nothing is cut off.
struct Section {
    std::string title, subtitle;
    std::vector<std::vector<std::string>> rows;
};

// Each section starts on a new page. With `checkbox_column`, column 0 of
// every body row is drawn as an empty tick box instead of text.
std::vector<uint8_t> sections_pdf(const std::vector<Section>& sections,
                                  const std::vector<std::string>& headers,
                                  const std::vector<double>& col_widths, bool checkbox_column);

std::vector<uint8_t> table_pdf(const std::string& title, const std::string& subtitle,
                               const std::vector<std::string>& headers,
                               const std::vector<double>& col_widths,
                               const std::vector<std::vector<std::string>>& rows) {
    return sections_pdf({{title, subtitle, rows}}, headers, col_widths, false);
}

std::vector<uint8_t> sections_pdf(const std::vector<Section>& sections,
                                  const std::vector<std::string>& headers,
                                  const std::vector<double>& col_widths, bool checkbox_column) {
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

    // Wraps `text` (UTF-8) to `max_w` at `size` pt; returns WinAnsi lines.
    auto wrap = [](PoDoFo::PdfFont* font, const std::string& text, double size, double max_w) {
        return pdf_text::wrap_lines(pdf_text::to_winansi(text), max_w, [&](const std::string& t) {
            font->SetFontSize(static_cast<float>(size));
            return static_cast<double>(font->GetFontMetrics()->StringWidth(t.c_str()));
        });
    };
    using Cells = std::vector<std::vector<std::string>>;  // each cell's lines
    auto wrap_cells = [&](PoDoFo::PdfFont* font, const std::vector<std::string>& cells,
                          bool checkbox) {
        Cells out;
        for (size_t c = 0; c < cells.size() && c < col_widths.size(); ++c) {
            out.push_back(c == 0 && checkbox ? std::vector<std::string>{}
                                             : wrap(font, cells[c], 9.0, col_widths[c] - 6));
        }
        return out;
    };
    auto height_of = [](const Cells& cells) {
        size_t lines = 1;
        for (const auto& c : cells) lines = std::max(lines, c.size());
        return report_row_height(lines);
    };

    const double row_h = kReportRowHeight;
    const double line_h = kReportLineHeight;
    // Draws a row whose top is y + 4 (the band runs from there down `h`).
    auto draw_row = [&](PoDoFo::PdfPainter& painter, PoDoFo::PdfFont* font, double y,
                        const Cells& cells, bool checkbox) {
        font->SetFontSize(9.0f);
        painter.SetFont(font);
        double x = margin;
        for (size_t c = 0; c < cells.size(); ++c) {
            if (c == 0 && checkbox) {
                painter.SetStrokeWidth(0.8);
                painter.Rectangle(x + 4, y - row_h + 6, 8, 8);
                painter.Stroke();
            } else {
                double baseline = y - row_h + 8;
                for (const auto& line : cells[c]) {
                    painter.DrawText(x + 3, baseline, PoDoFo::PdfString(line.c_str()));
                    baseline -= line_h;
                }
            }
            x += col_widths[c];
        }
    };
    auto band = [&](PoDoFo::PdfPainter& painter, double y, double h, double gray) {
        painter.SetColor(gray, gray, gray);
        painter.Rectangle(margin, y - h + 4, table_w, h);
        painter.Fill();
        painter.SetColor(0, 0, 0);
    };

    const Cells header_cells = wrap_cells(font_bold, headers, false);
    const double header_h = height_of(header_cells);
    for (const Section& section : sections) {
    // The heading (title, subtitle, a gap, the table header) repeats on
    // every page of the section; title and subtitle wrap to the table.
    const double text_w = std::max(table_w, 100.0);
    const auto title_lines = wrap(font_bold, section.title, 14.0, text_w);
    const auto subtitle_lines = wrap(font_regular, section.subtitle, 9.0, text_w);
    const double heading_h =
        row_h * static_cast<double>(title_lines.size() + subtitle_lines.size() + 1) + header_h;

    std::vector<Cells> rows;
    std::vector<double> heights;
    for (const auto& r : section.rows) {
        rows.push_back(wrap_cells(font_regular, r, checkbox_column));
        heights.push_back(height_of(rows.back()));
    }
    std::vector<int> pages = paginate_rows(heights, page_h - 2 * margin - heading_h);
    int total_pages = pages.empty() ? 1 : pages.back() + 1;

    size_t idx = 0;
    for (int page = 0; page < total_pages; ++page) {
        PoDoFo::PdfPage* pdf_page = doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
        PoDoFo::PdfPainter painter;
        painter.SetPage(pdf_page);

        double y = page_h - margin;

        font_bold->SetFontSize(14.0f);
        painter.SetFont(font_bold);
        for (const auto& line : title_lines) {
            painter.DrawText(margin, y - 14, PoDoFo::PdfString(line.c_str()));
            y -= row_h;
        }

        font_regular->SetFontSize(9.0f);
        painter.SetFont(font_regular);
        for (const auto& line : subtitle_lines) {
            painter.DrawText(margin, y - 10, PoDoFo::PdfString(line.c_str()));
            y -= row_h;
        }
        y -= row_h;

        band(painter, y, header_h, 0.85);
        draw_row(painter, font_bold, y, header_cells, false);
        y -= header_h;

        for (int r = 0; idx < rows.size() && pages[idx] == page; ++r, ++idx) {
            if (r % 2 == 1) band(painter, y, heights[idx], 0.96);
            draw_row(painter, font_regular, y, rows[idx], checkbox_column);
            y -= heights[idx];
        }

        painter.FinishPage();
    }
    }  // sections
    if (sections.empty()) {
        doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
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

std::vector<uint8_t> checklist_pdf(const std::vector<LabelRecord>& records) {
    std::map<std::string, std::vector<const LabelRecord*>> by_person;
    for (const auto& r : records) by_person[r.person].push_back(&r);
    std::vector<std::string> people;
    for (const auto& [person, _] : by_person) people.push_back(person);
    std::sort(people.begin(), people.end(), [](const std::string& a, const std::string& b) {
        return person_sort_key(a, SortBy::kLastName) < person_sort_key(b, SortBy::kLastName);
    });

    std::vector<Section> sections;
    for (const auto& person : people) {
        Section sec;
        sec.title = person;
        double pieces = 0;
        for (const LabelRecord* r : by_person[person]) {
            pieces += parse_qty(r->qty);
            std::string colors = r->lego_color;
            if (!r->bl_color.empty()) colors += (colors.empty() ? "" : " / ") + r->bl_color;
            std::string label = r->part_total > 0 ? std::to_string(r->part_seq) + " of " +
                                                        std::to_string(r->part_total)
                                                  : "";
            sec.rows.push_back({"", r->element_id, r->description, colors, r->qty, label});
        }
        sec.subtitle = std::to_string(by_person[person].size()) + " lots, " + format_g(pieces) +
                       " pieces";
        sections.push_back(std::move(sec));
    }
    return sections_pdf(sections, {"", "Element", "Description", "LEGO / BrickLink color", "Qty", "Label"},
                        {22, 55, 160, 145, 45, 55}, true);
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

std::vector<int> paginate_rows(const std::vector<double>& heights, double room) {
    std::vector<int> pages;
    int page = 0;
    double used = 0;
    for (double h : heights) {
        if (used > 0 && used + h > room + 1e-9) {
            ++page;
            used = 0;
        }
        pages.push_back(page);
        used += h;
    }
    return pages;
}

}  // namespace lugbulk::reports

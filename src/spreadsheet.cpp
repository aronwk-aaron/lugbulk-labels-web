#include "spreadsheet.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>

namespace lugbulk::spreadsheet {

namespace {

// --- zip ------------------------------------------------------------------

uint32_t u16(const std::string& d, size_t at) {
    if (at + 2 > d.size()) throw Error("That .xlsx file is damaged.");
    return static_cast<uint8_t>(d[at]) | static_cast<uint8_t>(d[at + 1]) << 8;
}
uint32_t u32(const std::string& d, size_t at) { return u16(d, at) | u16(d, at + 2) << 16; }

struct Entry {
    std::string name;
    uint16_t method;
    uint32_t compressed, size, local_offset;
};

std::vector<Entry> central_directory(const std::string& d) {
    // End of central directory: signature 0x06054b50 within the last 64 KB.
    if (d.size() < 22) throw Error("That file isn't a valid .xlsx workbook.");
    size_t min = d.size() > 65557 ? d.size() - 65557 : 0;
    size_t eocd = std::string::npos;
    for (size_t i = d.size() - 22;; --i) {
        if (u32(d, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == min) break;
    }
    if (eocd == std::string::npos) throw Error("That file isn't a valid .xlsx workbook.");
    uint32_t count = u16(d, eocd + 10), offset = u32(d, eocd + 16);
    if (count > 10000) throw Error("That .xlsx file has too many parts.");
    std::vector<Entry> entries;
    size_t at = offset;
    for (uint32_t i = 0; i < count; ++i) {
        if (u32(d, at) != 0x02014b50) throw Error("That .xlsx file is damaged.");
        Entry e;
        e.method = static_cast<uint16_t>(u16(d, at + 10));
        e.compressed = u32(d, at + 20);
        e.size = u32(d, at + 24);
        uint32_t name_len = u16(d, at + 28), extra = u16(d, at + 30), comment = u16(d, at + 32);
        e.local_offset = u32(d, at + 42);
        if (at + 46 + name_len > d.size()) throw Error("That .xlsx file is damaged.");
        e.name = d.substr(at + 46, name_len);
        entries.push_back(e);
        at += 46 + name_len + extra + comment;
    }
    return entries;
}

std::string extract(const std::string& d, const Entry& e, size_t& budget) {
    size_t at = e.local_offset;
    if (u32(d, at) != 0x04034b50) throw Error("That .xlsx file is damaged.");
    size_t data_at = at + 30 + u16(d, at + 26) + u16(d, at + 28);
    if (data_at + e.compressed > d.size()) throw Error("That .xlsx file is damaged.");
    if (e.size > budget) throw Error("That .xlsx file unpacks to more than 64 MB — too big.");
    if (e.method == 0) {
        budget -= e.compressed;
        return d.substr(data_at, e.compressed);
    }
    if (e.method != 8) throw Error("That .xlsx file uses an unsupported compression.");

    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) throw Error("Couldn't read that .xlsx file.");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(d.data() + data_at));
    zs.avail_in = e.compressed;
    std::string out;
    char buf[65536];
    int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        size_t got = sizeof(buf) - zs.avail_out;
        // Enforce the budget on what actually comes out, not the header's
        // claimed size — a zip bomb can lie about that.
        if (got > budget) {
            inflateEnd(&zs);
            throw Error("That .xlsx file unpacks to more than 64 MB — too big.");
        }
        budget -= got;
        out.append(buf, got);
    } while (rc == Z_OK);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) throw Error("That .xlsx file is damaged.");
    return out;
}

// --- minimal XML scanning ---------------------------------------------------
// The parts of SpreadsheetML we need are flat and regular, so a tag scanner
// is enough (no DTDs, no entities beyond the predefined and numeric ones).

std::string decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (!ent.empty() && ent[0] == '#') {
            cp = static_cast<uint32_t>(std::strtoul(ent.c_str() + (ent[1] == 'x' ? 2 : 1), nullptr,
                                                    ent[1] == 'x' ? 16 : 10));
        } else {
            out.append(s, i, semi - i + 1);
            i = semi;
            continue;
        }
        // UTF-8 encode
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        i = semi;
    }
    return out;
}

// Value of attribute `name` in a start tag (the text between '<' and '>').
std::optional<std::string> attr(const std::string& tag, const std::string& name) {
    size_t at = 0;
    while ((at = tag.find(name + "=", at)) != std::string::npos) {
        bool boundary = at == 0 || std::isspace(static_cast<unsigned char>(tag[at - 1]));
        size_t q = at + name.size() + 1;
        if (boundary && q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
            size_t end = tag.find(tag[q], q + 1);
            if (end == std::string::npos) return std::nullopt;
            return decode_entities(tag.substr(q + 1, end - q - 1));
        }
        at = q;
    }
    return std::nullopt;
}

// Calls f(tag_text, body) for each <name ...>body</name> (or <name .../>)
// element. Matches the local name, with or without a namespace prefix.
template <typename F>
void each_element(const std::string& xml, const std::string& name, F f) {
    size_t at = 0;
    while ((at = xml.find('<', at)) != std::string::npos) {
        size_t name_start = at + 1;
        size_t tag_end = xml.find('>', at);
        if (tag_end == std::string::npos) return;
        size_t name_end = xml.find_first_of(" \t\r\n/>", name_start);
        std::string qname = xml.substr(name_start, name_end - name_start);
        size_t colon = qname.find(':');
        std::string local = colon == std::string::npos ? qname : qname.substr(colon + 1);
        if (local != name || xml[name_start] == '/') {
            at = tag_end + 1;
            continue;
        }
        std::string tag = xml.substr(name_start, tag_end - name_start);
        if (!tag.empty() && tag.back() == '/') {
            f(tag, std::string());
            at = tag_end + 1;
            continue;
        }
        std::string close = "</" + qname + ">";
        size_t body_end = xml.find(close, tag_end + 1);
        if (body_end == std::string::npos) return;
        f(tag, xml.substr(tag_end + 1, body_end - tag_end - 1));
        at = body_end + close.size();
    }
}

// All <t> text inside `xml`, concatenated (a rich-text string is several runs).
std::string text_of(const std::string& xml) {
    std::string out;
    each_element(xml, "t", [&](const std::string&, const std::string& body) {
        out += decode_entities(body);
    });
    return out;
}

// "BC12" -> 54 (0-based column)
size_t column_index(const std::string& ref) {
    size_t col = 0;
    for (char c : ref) {
        if (!std::isalpha(static_cast<unsigned char>(c))) break;
        col = col * 26 + (std::toupper(static_cast<unsigned char>(c)) - 'A' + 1);
    }
    return col == 0 ? 0 : col - 1;
}

// Numbers as a person would type them: 4211407.0 -> "4211407", 0.83 -> "0.83".
std::string format_number(const std::string& raw) {
    char* end = nullptr;
    double v = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str()) return raw;
    if (std::fabs(v) < 1e15 && v == std::floor(v)) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

Rows read_sheet(const std::string& xml, const std::vector<std::string>& shared) {
    Rows rows;
    each_element(xml, "row", [&](const std::string& row_tag, const std::string& row_body) {
        size_t r = rows.size();
        if (auto ref = attr(row_tag, "r")) r = std::max<long>(1, std::atol(ref->c_str())) - 1;
        if (r > 100000) return;  // far beyond any real order sheet
        if (rows.size() <= r) rows.resize(r + 1);
        auto& row = rows[r];
        each_element(row_body, "c", [&](const std::string& c_tag, const std::string& c_body) {
            size_t col = row.size();
            if (auto ref = attr(c_tag, "r")) col = column_index(*ref);
            if (col > 2000) return;
            std::string type = attr(c_tag, "t").value_or("n");
            std::string value;
            if (type == "inlineStr") {
                value = text_of(c_body);
            } else {
                std::string v;
                each_element(c_body, "v", [&](const std::string&, const std::string& body) {
                    v = decode_entities(body);
                });
                if (type == "s") {
                    size_t i = static_cast<size_t>(std::atol(v.c_str()));
                    value = i < shared.size() ? shared[i] : "";
                } else if (type == "n") {
                    value = v.empty() ? "" : format_number(v);
                } else if (type == "b") {
                    value = v == "1" ? "TRUE" : "FALSE";
                } else {
                    value = v;  // "str" (formula result), "e" (error)
                }
            }
            if (row.size() <= col) row.resize(col + 1);
            row[col] = value;
        });
    });
    return rows;
}

std::string normalize(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (!std::isspace(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

}  // namespace

bool is_xlsx(const std::string& data) { return data.rfind("PK\x03\x04", 0) == 0; }

Rows read_csv(const std::string& input) {
    std::string data = input.rfind("\xEF\xBB\xBF", 0) == 0 ? input.substr(3) : input;
    Rows rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < data.size(); ++i) {
        char c = data[i];
        if (quoted) {
            if (c == '"' && i + 1 < data.size() && data[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field.push_back(c);
            }
        } else if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == ',') {
            row.push_back(std::move(field));
            field.clear();
            any = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < data.size() && data[i + 1] == '\n') ++i;
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
            any = false;
        } else {
            field.push_back(c);
            any = true;
        }
    }
    if (any || !field.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<Rows> read_xlsx(const std::string& data, const std::string& tab) {
    std::vector<Entry> entries = central_directory(data);
    size_t budget = kMaxUnpackedBytes;
    auto part = [&](const std::string& name) -> std::optional<std::string> {
        for (const auto& e : entries) {
            if (e.name == name) return extract(data, e, budget);
        }
        return std::nullopt;
    };

    auto workbook = part("xl/workbook.xml");
    auto rels = part("xl/_rels/workbook.xml.rels");
    if (!workbook || !rels) throw Error("That file isn't an Excel workbook.");

    std::vector<std::string> shared;
    if (auto sst = part("xl/sharedStrings.xml")) {
        each_element(*sst, "si", [&](const std::string&, const std::string& body) {
            shared.push_back(text_of(body));
        });
    }

    std::map<std::string, std::string> targets;  // relationship id -> part path
    each_element(*rels, "Relationship", [&](const std::string& tag, const std::string&) {
        auto id = attr(tag, "Id"), target = attr(tag, "Target");
        if (!id || !target) return;
        std::string path = *target;
        if (path.rfind("/", 0) == 0) path = path.substr(1);
        else if (path.rfind("xl/", 0) != 0) path = "xl/" + path;
        targets[*id] = path;
    });

    std::vector<std::pair<std::string, std::string>> sheets;  // (name, part path)
    each_element(*workbook, "sheet", [&](const std::string& tag, const std::string&) {
        auto name = attr(tag, "name"), rid = attr(tag, "r:id");
        if (name && rid && targets.count(*rid)) sheets.emplace_back(*name, targets[*rid]);
    });
    if (sheets.empty()) throw Error("That workbook has no sheets.");

    std::vector<Rows> out;
    for (const auto& [name, path] : sheets) {
        if (normalize(name) == normalize(tab)) {
            if (auto xml = part(path)) out.push_back(read_sheet(*xml, shared));
            return out;
        }
    }
    for (const auto& [name, path] : sheets) {
        if (auto xml = part(path)) out.push_back(read_sheet(*xml, shared));
    }
    return out;
}

}  // namespace lugbulk::spreadsheet

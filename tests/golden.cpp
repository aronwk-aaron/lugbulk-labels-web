// Writes golden JSON files for the browser port's parity test
// (tests/js/parity.test.mjs): what the C++ pivot/ordering/colors/records
// code makes of each fixture in tests/fixtures (and a few generated cases),
// together with the input rows it read, so the JavaScript in static/js/
// can run on the same rows and must give deep-equal results.
//
//   lugbulk_golden <out dir>
//
// Writes <out dir>/case-<name>.json per sheet, <out dir>/units.json for
// the smaller functions, and <out dir>/spreadsheets.json: what the .xlsx and
// .csv reader makes of each fixture file and of generated inputs. Every fixture is invented data (no real orders).

#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "crow/json.h"

#include "bricklink.h"
#include "colors.h"
#include "ordering.h"
#include "records.h"
#include "reports.h"
#include "sheet_layout.h"
#include "sheet_pivot.h"
#include "spreadsheet.h"

using namespace lugbulk;

namespace {

using Rows = std::vector<std::vector<std::string>>;

std::string jstr(const std::string& s) {
    std::string out;
    crow::json::escape(s, out);
    return "\"" + out + "\"";
}

// Round-trips exactly through JSON.parse; non-finite numbers become null
// (as JSON.stringify does on the JavaScript side).
std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string jopt(const std::optional<double>& v) { return v ? jnum(*v) : "null"; }

std::string jbool(bool b) { return b ? "true" : "false"; }

template <typename T, typename F>
std::string jarr(const std::vector<T>& items, F each) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += each(items[i]);
    }
    return out + "]";
}

std::string rows_json(const Rows& rows) {
    return jarr(rows, [](const std::vector<std::string>& row) { return jarr(row, jstr); });
}

// image_url is left out: the browser uses its own /img/<id>.jpg (checked
// separately by the JS test).
std::string record_json(const LabelRecord& r) {
    return "{\"person\":" + jstr(r.person) + ",\"element_id\":" + jstr(r.element_id) +
           ",\"description\":" + jstr(r.description) + ",\"lego_color\":" + jstr(r.lego_color) +
           ",\"bl_color\":" + jstr(r.bl_color) + ",\"qty\":" + jstr(r.qty) +
           ",\"weight\":" + jopt(r.weight) + ",\"catalog_weight\":" + jopt(r.catalog_weight) +
           ",\"part_seq\":" + std::to_string(r.part_seq) +
           ",\"part_total\":" + std::to_string(r.part_total) + "}";
}

std::string issue_json(const SheetIssue& i) {
    return "{\"row\":" + std::to_string(i.row) + ",\"kind\":" + jstr(i.kind) +
           ",\"detail\":" + jstr(i.detail) + ",\"element_id\":" + jstr(i.element_id) + "}";
}

std::string pivot_json(const PivotResult& p) {
    return "{\"records\":" + jarr(p.records, record_json) +
           ",\"issues\":" + jarr(p.issues, issue_json) + "}";
}

std::string part_json(const ordering::PartSummary& p) {
    return "{\"element_id\":" + jstr(p.element_id) + ",\"description\":" + jstr(p.description) +
           ",\"lego_color\":" + jstr(p.lego_color) + ",\"bl_color\":" + jstr(p.bl_color) +
           ",\"lots\":" + std::to_string(p.lots) + ",\"pieces\":" + jnum(p.pieces) +
           ",\"weight\":" + jopt(p.weight) + ",\"weight_source\":" + jstr(p.weight_source) + "}";
}

std::vector<std::string> distinct_ids(const PivotResult& p) {
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const auto& r : p.records) {
        if (seen.insert(r.element_id).second) ids.push_back(r.element_id);
    }
    return ids;
}

// A made-up BrickLink catalog over a case's parts, varied so every branch
// of apply_bricklink is hit: some parts unknown, some with a color and a
// weight, some with neither, some with a color the table doesn't know.
bricklink::Catalog fake_catalog(const std::vector<std::string>& ids) {
    bricklink::Catalog catalog;
    for (size_t i = 0; i < ids.size(); ++i) {
        switch (i % 4) {
            case 0: break;  // not in the catalog
            case 1: catalog[ids[i]] = {"p" + ids[i], "Light Bluish Gray", 1.25}; break;
            case 2: catalog[ids[i]] = {"p" + ids[i], "", std::nullopt}; break;
            case 3:
                catalog[ids[i]] = {"p" + ids[i], i % 8 == 3 ? "Weird Catalog Color" : "Trans-Clear",
                                   0.5 + 0.01 * static_cast<double>(i)};
                break;
        }
    }
    catalog["99999999"] = {"9999", "Black", 3.0};  // not on the sheet
    return catalog;
}

std::string case_json(const std::vector<Rows>& tabs) {
    std::string out = "{\"tabs\":" + jarr(tabs, rows_json);

    // records::pivot_tabs without the size check, to see the pivot itself.
    PivotResult pivot;
    for (Rows rows : tabs) {
        records::cap_rows(rows);
        pivot = pivot_sheet(rows);
        if (!pivot.records.empty()) break;
    }
    out += ",\"pivot\":" + pivot_json(pivot);
    try {
        records::pivot_tabs(tabs);
        out += ",\"too_big\":null";
    } catch (const records::TooBig& e) {
        out += ",\"too_big\":" + jstr(e.what());
        return out + "}";
    }

    std::vector<std::string> ids = distinct_ids(pivot);
    bricklink::Catalog catalog = fake_catalog(ids);
    out += ",\"lookup_ids\":" + jarr(ids, jstr);
    out += ",\"lookup\":" + bricklink::lookup_json(catalog, ids);
    records::apply_bricklink(pivot, catalog);
    out += ",\"applied\":" + pivot_json(pivot);
    out += ",\"check\":" + records::check_summary_json(pivot);

    out += ",\"orders\":{";
    const std::pair<const char*, ordering::PartOrder> orders[] = {
        {"heaviest", ordering::PartOrder::kHeaviest},
        {"lightest", ordering::PartOrder::kLightest},
        {"sheet", ordering::PartOrder::kSheet}};
    bool first = true;
    for (const auto& [name, order] : orders) {
        if (!first) out += ",";
        first = false;
        out += jstr(name) + ":{\"records\":" +
               jarr(ordering::order_records(pivot.records, order), record_json) +
               ",\"parts\":" + jarr(ordering::summarize_parts(pivot.records, order), part_json) + "}";
    }
    return out + "}}";
}

// Over the row cap (3000) and the part limit (2000): 3100 rows of distinct parts.
std::vector<Rows> too_many_parts() {
    Rows rows{{"#", "Element ID", "Photo", "Description", "BL Color", "Pat Example", "$$"},
              {"", "", "", "", "", "qty", "$$"}};
    for (int i = 0; i < 3100; ++i) {
        rows.push_back({std::to_string(i + 1), std::to_string(1000000 + i), "",
                        "PLATE 1X" + std::to_string(i % 8 + 1), "White", "1", ""});
    }
    return {rows};
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary);
    out << body << "\n";
    if (!out) throw std::runtime_error("can't write " + path);
    std::cout << "wrote " << path << "\n";
}

// ---- units.json -----------------------------------------------------------

const char* const kColorNames[] = {
    "White", "WHITE", "Black", "BLACK", "Bright Red", "Red", "BR.RED", "BR. RED", "Bright Blue",
    "Blue", "BR.BLUE", "BR. BLUE", "Bright Yellow", "Yellow", "BR.YEL", "BR. YEL", "BR.YELLOW",
    "Bright Green", "BR.GREEN", "BR. GREEN", "Dark Green", "Green", "DK.GREEN", "DK. GREEN",
    "Earth Green", "EARTH GREEN", "Earth Blue", "Dark Blue", "EARTH BLUE", "Medium Stone Grey",
    "Light Bluish Gray", "MED. ST-GREY", "MED.ST-GREY", "MED. ST. GREY", "M. ST. GREY",
    "LT. ST. GREY", "LT.ST.GREY", "Dark Stone Grey", "Dark Bluish Gray", "DK. ST. GREY",
    "DK.ST.GREY", "DK. ST-GREY", "Brick Yellow", "Tan", "BRICK-YEL", "BRICK YEL", "BRICK-YELLOW",
    "Sand Yellow", "Dark Tan", "SAND YELLOW", "Reddish Brown", "RED. BROWN", "RED.BROWN",
    "Dark Brown", "DK. BROWN", "DK.BROWN", "New Dark Red", "Dark Red", "NEW DARK RED",
    "Sand Green", "SAND GREEN", "Sand Blue", "SAND BLUE", "Olive Green", "OLIVE GREEN", "Nougat",
    "NOUGAT", "Medium Nougat", "M. NOUGAT", "MED. NOUGAT", "M.NOUGAT", "Light Nougat",
    "L.NOUGAT", "L. NOUGAT", "LGH. NOUGAT", "Dark Orange", "DK.ORA", "DK. ORA", "DK.ORANGE",
    "Bright Orange", "Orange", "BR.ORANGE", "BR. ORANGE", "BR.ORA", "Reddish Orange",
    "RED. ORANGE", "RED.ORANGE", "Flame Yellowish Orange", "Bright Light Orange", "FL. YELL-ORA",
    "FL.YELL-ORA", "Bright Yellowish Green", "Lime", "BR.YEL-GREEN", "BR. YEL-GREEN",
    "Bright Bluish Green", "Dark Turquoise", "BR.BLUEGREEN", "BR. BLUEGREEN", "Aqua",
    "Light Aqua", "AQUA", "Lavender", "LAVENDER", "Medium Lavender", "M. LAVENDER",
    "MED. LAVENDER", "Medium Lilac", "Dark Purple", "MEDIUM LILAC", "M. LILAC",
    "Bright Reddish Violet", "Magenta", "BR.RED-VIOLET", "BR.RED.VIOLET", "Bright Purple",
    "Dark Pink", "BR.PURPLE", "BR. PURPLE", "Light Purple", "Bright Pink", "LGH. PURPLE",
    "LGH.PURPLE", "Medium Blue", "MEDIUM BLUE", "M. BLUE", "Medium Azur", "Medium Azure",
    "MEDIUM AZUR", "MED. AZUR", "Dark Azur", "Dark Azure", "DARK AZUR", "DK. AZUR",
    "Light Royal Blue", "Bright Light Blue", "LT.ROY.BLUE", "LGH. ROYAL BLUE", "Cool Yellow",
    "Bright Light Yellow", "COOL YELLOW", "Vibrant Coral", "Coral", "VIBRANT CORAL", "Warm Pink",
    "WARM PINK", "Spring Yellowish Green", "Yellowish Green", "SPR. YEL-GREEN",
    "Silver Metallic", "Flat Silver", "SILVER MET.", "SILVER MET", "Titanium Metallic",
    "Pearl Dark Gray", "TITAN. MET.", "TITANIUM MET.", "Warm Gold", "Pearl Gold", "WARM GOLD",
    "White Glow", "Glow In Dark White", "WHITE GLOW", "Transparent", "Trans-Clear", "TR.", "TR",
    "TRANSPARENT", "Transparent Light Blue", "Trans-Light Blue", "TR.L.BLUE", "TR. L. BLUE",
    "Transparent Blue", "Trans-Dark Blue", "TR.BLUE", "TR. BLUE", "Transparent Brown",
    "Trans-Black", "TR.BROWN", "TR. BROWN", "Transparent Red", "Trans-Red", "TR.RED", "TR. RED",
    "Transparent Green", "Trans-Green", "TR.GREEN", "TR. GREEN", "Transparent Yellow",
    "Trans-Yellow", "TR.YEL", "TR. YELLOW", "TR.YELLOW", "Transparent Bright Orange",
    "Trans-Orange", "TR.BR.ORANGE", "Transparent Fluorescent Reddish Orange",
    "Trans-Neon Orange", "TR.FL.RED-ORA", "Transparent Fluorescent Green", "Trans-Neon Green",
    "TR.FL.GREEN",
    // Not in the table as written:
    "", " ", "  light bluish gray  ", "white!", "Weird Pink", "trans clear", "Trans Purple",
    "\xc3\x89" "cru", "bright  red", "tr", "t", "Glow-in-dark", "MED ST GREY", "\tBlack\t"};

std::string color_case(const std::string& lego, const std::string& bl) {
    colors::Resolved r = colors::resolve(lego, bl);
    auto rgb = colors::swatch_rgb(lego, bl);
    std::string swatch = "null";
    if (rgb) swatch = "[" + jnum((*rgb)[0]) + "," + jnum((*rgb)[1]) + "," + jnum((*rgb)[2]) + "]";
    return "{\"lego_in\":" + jstr(lego) + ",\"bl_in\":" + jstr(bl) + ",\"lego\":" + jstr(r.lego) +
           ",\"bl\":" + jstr(r.bl) + ",\"mapped\":" + jbool(r.mapped) +
           ",\"transparent\":" + jbool(colors::is_transparent(lego, bl)) +
           ",\"light\":" + jbool(colors::is_light(lego, bl)) + ",\"swatch\":" + swatch + "}";
}

std::string units_json() {
    std::vector<std::string> color_cases;
    std::vector<std::string> names(std::begin(kColorNames), std::end(kColorNames));
    for (const auto& n : names) {
        color_cases.push_back(color_case(n, ""));
        color_cases.push_back(color_case("", n));
    }
    // Both sides filled in.
    for (size_t i = 0; i + 7 < names.size(); i += 7) color_cases.push_back(color_case(names[i], names[i + 7]));
    color_cases.push_back(color_case("Mystery", "Trans-Clear"));
    color_cases.push_back(color_case("MED. ST-GREY", "Custom"));

    const std::vector<std::string> qtys{
        "2,000", "150", " 150 ", "1.5", "abc", "", "  ", ",", "1e3", "1E3", "1e", "0x10", "0X1f",
        "0x", "0x1p3", "0x1.8p1", "+5", "-3", ".5", "5.", ".", "inf", "INF", "-Infinity", "nan",
        "NaN(123)", "1e400", "-1e400", "1e-400", "1e-310", "0e-400", "\v5", "5\v", "\f7", "1,2,3",
        "12abc", "1 000", "$5", "5g", "0", "-0", "00012", "0.0078125", "1e-7", "3.14159265358979",
        "12345678901234567890", "9007199254740993", "\xe2\x80\x87" "5"};
    std::vector<std::string> qty_cases;
    for (const auto& q : qtys) {
        std::string value;
        try {
            double v = parse_qty(q);
            value = std::isnan(v) ? "\"nan\"" : std::isinf(v) ? (v > 0 ? "\"inf\"" : "\"-inf\"") : jnum(v);
        } catch (const std::invalid_argument&) {
            value = "\"error\"";
        }
        qty_cases.push_back("[" + jstr(q) + "," + value + "]");
    }

    const std::vector<std::string> ids{"1234", "12345678", "123", "123456789", "12a4", "",
                                       " 1234", "1234 ", "../1234", "\xd9\xa1\xd9\xa2\xd9\xa3\xd9\xa4",
                                       "00000000", "+1234"};
    std::vector<std::string> id_cases;
    for (const auto& id : ids) id_cases.push_back("[" + jstr(id) + "," + jbool(is_valid_element_id(id)) + "]");

    const std::vector<std::string> descriptions{
        "BRICK 2X4", "PLATE 2X4", "TILE 1X1", "BRICK 1X2X5", "BRICK 1X1X1 2/3",
        "ROOF TILE 1X2X3/73\xc2\xb0", "PLATE 1X2X2/3", "DUPLO BRICK 2X4", "DUPLO PLATE 2X4",
        "MINIFIG HEAD", "FROG", "BASE PLATE 32X32", "brick 1 x 2 x 3", "Plate 4x8", "PLADE 2X2",
        "BRICK W. BOW 1X4X1 1/3", "PLATE 1X2 W/ CLIP", "TILEX 2X2", "2X2", "X2", "1X", "0X4 PLATE",
        "BRICK 1X1X1 2/0", "SLOPE 45 2X2", "CORNER PLATE 2X2X2/3", "BRICK\t1\tX\t2",
        "BRICK 1X2X3/3", "PLATE_2X2", "1X2X3 4/5 PLATE"};
    std::vector<std::string> estimate_cases;
    for (const auto& d : descriptions) {
        estimate_cases.push_back("[" + jstr(d) + "," + jopt(ordering::estimate_weight(d)) + "]");
    }

    const std::vector<std::string> people{"Ann Lee",     "ann lee",   "Bob Roe",  "Zed",
                                          "Amy  Zhu",    "amy zhu",   "\xc3\x89mile Roe",
                                          "Lee Ann",     "Bo \tRoe",  "x \xf0\x9f\x98\x80",
                                          "x \xef\xbd\x81", "O'Neil",  "de la Rosa Pat"};
    std::vector<std::string> sorted_people = people;
    std::stable_sort(sorted_people.begin(), sorted_people.end(),
                     [](const std::string& a, const std::string& b) {
                         return reports::person_sort_key(a, reports::SortBy::kLastName) <
                                reports::person_sort_key(b, reports::SortBy::kLastName);
                     });

    std::vector<std::string> spec_names;
    for (const auto& s : layout::label_specs()) {
        spec_names.push_back(s.id);
        spec_names.push_back(s.brand + " " + s.part);
        spec_names.push_back(s.part);
        for (const auto& e : s.equivalents) spec_names.push_back(e);
    }
    for (const char* n : {"nope", "", "avery-5162", "AVERY 5162", "5 1 6 2", "dymo 30252"}) {
        spec_names.push_back(n);
    }
    std::vector<std::string> spec_cases;
    for (const auto& n : spec_names) {
        const layout::LabelSpec* s = layout::find_label_spec(n);
        spec_cases.push_back("[" + jstr(n) + "," + (s ? jstr(s->id) : "null") + "]");
    }

    return "{\"colors\":" + jarr(color_cases, [](const std::string& s) { return s; }) +
           ",\"parse_qty\":" + jarr(qty_cases, [](const std::string& s) { return s; }) +
           ",\"element_ids\":" + jarr(id_cases, [](const std::string& s) { return s; }) +
           ",\"estimate_weight\":" + jarr(estimate_cases, [](const std::string& s) { return s; }) +
           ",\"people\":" + jarr(people, jstr) + ",\"people_sorted\":" + jarr(sorted_people, jstr) +
           ",\"label_specs\":" + layout::label_specs_json() +
           ",\"label_spec_lookups\":" + jarr(spec_cases, [](const std::string& s) { return s; }) +
           ",\"part_orders\":[" + jstr("heaviest") + "," + jstr("lightest") + "," + jstr("sheet") +
           "," + jstr("Heaviest") + "," + jstr("") + "," + jstr("weight") + "]" +
           ",\"part_orders_valid\":[" + jbool(ordering::parse_part_order("heaviest").has_value()) +
           "," + jbool(ordering::parse_part_order("lightest").has_value()) + "," +
           jbool(ordering::parse_part_order("sheet").has_value()) + "," +
           jbool(ordering::parse_part_order("Heaviest").has_value()) + "," +
           jbool(ordering::parse_part_order("").has_value()) + "," +
           jbool(ordering::parse_part_order("weight").has_value()) + "]}";
}

// ---- spreadsheets.json ----------------------------------------------------
// What spreadsheet.cpp reads from each fixture file and from generated
// inputs (odd CSV, hand-built workbooks, damaged zips): the tabs, or the
// error message. static/js/spreadsheet.js must read the same.

std::string hex(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

// A small zip writer for test workbooks: entries stored or deflated, and
// optionally with a false "uncompressed size" or method in the headers.
struct ZipPart {
    std::string name, data;
    bool deflate = true;
    std::optional<uint32_t> claimed_size;  // default: the real size
    std::optional<uint16_t> method;        // default: 8 or 0
    size_t drop_tail = 0;                  // bytes cut off the compressed data
};

void put16(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xff));
    out.push_back(static_cast<char>((v >> 8) & 0xff));
}
void put32(std::string& out, uint32_t v) {
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

std::string deflate_raw(const std::string& data) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    std::string out(deflateBound(&zs, data.size()), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    int rc = deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) throw std::runtime_error("deflate failed");
    return out;
}

std::string make_zip(const std::vector<ZipPart>& parts) {
    std::string out, central;
    for (const auto& p : parts) {
        std::string body = p.deflate ? deflate_raw(p.data) : p.data;
        if (p.drop_tail) body.resize(body.size() > p.drop_tail ? body.size() - p.drop_tail : 0);
        uint16_t method = p.method.value_or(p.deflate ? 8 : 0);
        uint32_t size = p.claimed_size.value_or(static_cast<uint32_t>(p.data.size()));
        uint32_t crc = crc32(0L, reinterpret_cast<const Bytef*>(p.data.data()),
                             static_cast<uInt>(p.data.size()));
        uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, method);
        put32(out, 0);  // time, date
        put32(out, crc);
        put32(out, static_cast<uint32_t>(body.size()));
        put32(out, size);
        put16(out, static_cast<uint32_t>(p.name.size()));
        put16(out, 0);
        out += p.name + body;

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0);
        put16(central, method);
        put32(central, 0);
        put32(central, crc);
        put32(central, static_cast<uint32_t>(body.size()));
        put32(central, size);
        put16(central, static_cast<uint32_t>(p.name.size()));
        put32(central, 0);  // extra, comment lengths
        put32(central, 0);  // disk, internal attrs
        put32(central, 0);  // external attrs
        put32(central, offset);
        central += p.name;
    }
    uint32_t central_offset = static_cast<uint32_t>(out.size());
    out += central;
    put32(out, 0x06054b50);
    put32(out, 0);
    put16(out, static_cast<uint32_t>(parts.size()));
    put16(out, static_cast<uint32_t>(parts.size()));
    put32(out, static_cast<uint32_t>(central.size()));
    put32(out, central_offset);
    put16(out, 0);
    return out;
}

const char* const kWorkbookXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>"
    "<sheet name=\"Notes\" sheetId=\"1\" r:id=\"rId1\"/>"
    "<sheet name=\" order  HERE \" sheetId=\"2\" r:id=\"rId2\"/></sheets></workbook>";

const char* const kRelsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships "
    "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"worksheet\" Target=\"worksheets/sheet1.xml\"/>"
    "<Relationship Id='rId2' Type='worksheet' Target='/xl/worksheets/sheet2.xml'/>"
    "<Relationship Id=\"rId3\" Type=\"sharedStrings\" Target=\"sharedStrings.xml\"/>"
    "</Relationships>";

const char* const kSharedStringsXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><sst count=\"7\" uniqueCount=\"7\">"
    "<si><t>Element ID</t></si>"
    "<si><r><rPr><b/></rPr><t>Ann</t></r><r><t xml:space=\"preserve\"> Lee</t></r></si>"
    "<si><t>BRICK 2X4 &amp; more &lt;x&gt; &quot;q&quot; &apos;a&apos;</t></si>"
    "<si><t>Caf&#233; &#x1F600; &#; &#xZZ; &bogus; &averyveryverylongentity; &amp</t></si>"
    "<si><t/></si>"
    "<si><t>\xc3\x89mile \xc3\x91o\xc3\xb1o</t></si>"
    "<si><t>  padded  </t><rPh><t>phonetic</t></rPh></si>"
    "</sst>";

const char* const kNotesSheetXml =
    "<worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>not this tab</t></is>"
    "</c></row></sheetData></worksheet>";

const char* const kOrderSheetXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<worksheet><sheetData>"
    "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"C1\" t=\"s\"><v>1</v></c>"
    "<c r=\"B1\" t=\"inlineStr\"><is><t>inline &amp; text</t></is></c></row>"
    "<row r=\"3\" spans=\"1:20\"><c r=\"A3\"><v>4211407.0</v></c><c r=\"B3\"><v>0.83</v></c>"
    "<c r=\"C3\"><v>1.2345678901234567E+20</v></c><c r=\"D3\"><v>1234567890123465</v></c>"
    "<c r=\"E3\"><v>-0</v></c><c r=\"F3\"><v>1E-7</v></c><c r=\"G3\"><v>abc</v></c>"
    "<c r=\"H3\"><v> 12abc</v></c><c r=\"I3\"><v>0x1A</v></c><c r=\"J3\"><v>inf</v></c>"
    "<c r=\"K3\"><v>-nan</v></c><c r=\"L3\"><v>0.1</v></c><c r=\"M3\"><v>2.5E-5</v></c>"
    "<c r=\"N3\"><v>123456789.123456789</v></c><c r=\"O3\"><v></v></c><c r=\"P3\"/>"
    "<c r=\"Q3\"><v>1e400</v></c><c r=\"R3\"><v>100000000000000000000</v></c>"
    "<c r=\"S3\"><v>0.000123456789012345678</v></c><c r=\"T3\"><v>99999999999999.99</v></c>"
    "<c r=\"U3\"><v>-12.5</v></c><c r=\"V3\"><v>1e15</v></c><c r=\"W3\"><v>0.00001</v></c>"
    "<c r=\"X3\"><v>4.9999999999999996E-2</v></c><c r=\"Y3\"><v>1.</v></c></row>"
    "<row r=\"2\"><c r=\"A2\" t=\"b\"><v>1</v></c><c r=\"B2\" t=\"b\"><v>0</v></c>"
    "<c r=\"C2\" t=\"str\"><v>formula &amp; result</v></c><c r=\"D2\" t=\"e\"><v>#N/A</v></c>"
    "<c r=\"E2\" t=\"s\"><v>99</v></c><c r=\"F2\" t=\"s\"><v>-1</v></c>"
    "<c r=\"G2\" t=\"s\"><v>2</v></c><c r=\"H2\" t=\"s\"><v>3</v></c>"
    "<c r=\"I2\" t=\"s\"><v>5</v></c><c r=\"J2\" t=\"s\"><v>6</v></c><c r=\"K2\" t=\"s\"><v>4</v></c></row>"
    "<row><c><v>7</v></c><c><v>8</v></c><c r=\"ZZZ4\"><v>9</v></c><c r='e4' t='s'><v>1</v></c></row>"
    "<row r=\"100002\"><c r=\"A100002\"><v>1</v></c></row>"
    "<row r=\"0\"><c r=\"A1\"><v>overwrite</v></c></row>"
    "<x:row r=\"6\"><x:c r=\"B6\" t=\"s\"><x:v>1</x:v></x:c><x:c r=\"CV6\"><x:v>2</x:v></x:c></x:row>"
    "<row r=\"8\"><c r=\"A8\" t=\"inlineStr\"><is><r><t>rich</t></r><r><t> inline</t></r></is></c>"
    "<c r=\"B8\"><f>SUM(A1:A2)</f><v>3</v></c></row>"
    "</sheetData></worksheet>";

std::vector<ZipPart> order_workbook_parts() {
    return {{"[Content_Types].xml", "<Types/>"},
            {"xl/workbook.xml", kWorkbookXml},
            {"xl/_rels/workbook.xml.rels", kRelsXml},
            {"xl/sharedStrings.xml", kSharedStringsXml},
            {"xl/worksheets/sheet1.xml", kNotesSheetXml},
            {"xl/worksheets/sheet2.xml", kOrderSheetXml}};
}

std::vector<std::pair<std::string, std::string>> generated_inputs() {
    std::vector<std::pair<std::string, std::string>> in;
    // CSV
    in.push_back({"csv-quotes", "a,b\r\nc,\"d,\"\"e\"\"\"\n\"multi\nline\",x\r\"\",end"});
    in.push_back({"csv-bom", "\xEF\xBB\xBFhead,er\n1,2\n"});
    in.push_back({"csv-empty", ""});
    in.push_back({"csv-blank-lines", "\n\n\r\n"});
    in.push_back({"csv-trailing", "trailing,\n,\n,"});
    in.push_back({"csv-utf8", "\xc3\x89mile,\"Zo\xc3\xab\"\"s\"\r\n\xe2\x82\xac" "5,\xf0\x9f\x98\x80"});
    in.push_back({"csv-unterminated", "\"unterminated,quote\nstill"});
    in.push_back({"csv-mid-quote", "a\"b,c\"d\ne"});
    in.push_back({"csv-double-bom", "\xEF\xBB\xBF\xEF\xBB\xBFx"});
    // Workbooks
    in.push_back({"xlsx-order-tab", make_zip(order_workbook_parts())});
    {
        auto parts = order_workbook_parts();
        for (auto& p : parts) p.deflate = false;
        in.push_back({"xlsx-stored", make_zip(parts)});
    }
    in.push_back({"xlsx-no-order-tab",
                  make_zip({{"xl/workbook.xml",
                             "<workbook><sheets><sheet name=\"\xc3\x9c" "bersicht\" r:id=\"rId1\"/>"
                             "<sheet name=\"Ghost\" r:id=\"rId9\"/><sheet name=\"Missing\" r:id=\"rId3\"/>"
                             "<sheet r:id=\"rId2\"/><sheet name=\"Data\" r:id=\"rId2\"/></sheets></workbook>"},
                            {"xl/_rels/workbook.xml.rels",
                             "<Relationships><Relationship Id=\"rId1\" Target=\"worksheets/a.xml\"/>"
                             "<Relationship Id=\"rId2\" Target=\"xl/worksheets/b.xml\"/>"
                             "<Relationship Id=\"rId3\" Target=\"worksheets/missing.xml\"/>"
                             "<Relationship Target=\"worksheets/none.xml\"/></Relationships>"},
                            {"xl/worksheets/a.xml", "<worksheet><sheetData/></worksheet>", false},
                            {"xl/worksheets/b.xml",
                             "<worksheet><sheetData><row r=\"2\"><c r=\"B2\" t=\"s\"><v>0</v></c>"
                             "<c r=\"C2\"><v>3.5</v></c></row></sheetData></worksheet>"}})});
    in.push_back({"xlsx-no-sheets",
                  make_zip({{"xl/workbook.xml", "<workbook><sheets/></workbook>"},
                            {"xl/_rels/workbook.xml.rels", "<Relationships/>"}})});
    in.push_back({"xlsx-no-rels", make_zip({{"xl/workbook.xml", kWorkbookXml}})});
    in.push_back({"xlsx-not-a-zip", "PK\x03\x04 not really a zip, just text that starts like one"});
    in.push_back({"xlsx-tiny", std::string("PK\x03\x04", 4)});
    {
        auto parts = order_workbook_parts();
        parts[1].method = 12;  // bzip2
        in.push_back({"xlsx-unsupported", make_zip(parts)});
    }
    {
        // A sheet that inflates past 64 MB while its header claims 100 bytes.
        auto parts = order_workbook_parts();
        parts[5].data = "<worksheet><sheetData>" + std::string(spreadsheet::kMaxUnpackedBytes, ' ') + "</sheetData></worksheet>";
        parts[5].claimed_size = 100;
        in.push_back({"xlsx-lying-bomb", make_zip(parts)});
    }
    {
        auto parts = order_workbook_parts();
        parts[2].claimed_size = 0x7fffffff;  // claims 2 GB: refused before inflating
        in.push_back({"xlsx-claims-huge", make_zip(parts)});
    }
    {
        auto parts = order_workbook_parts();
        parts[5].drop_tail = 20;  // truncated deflate data
        in.push_back({"xlsx-truncated-part", make_zip(parts)});
    }
    {
        std::string z = make_zip(order_workbook_parts());
        std::string bad = z;
        bad[bad.size() - 22 + 10] = static_cast<char>(0x11);  // 10001 parts
        bad[bad.size() - 22 + 11] = static_cast<char>(0x27);
        in.push_back({"xlsx-too-many-parts", bad});
        bad = z;
        bad[bad.find("PK\x01\x02") + 3] = 0x03;
        in.push_back({"xlsx-bad-central", bad});
        in.push_back({"xlsx-cut-short", z.substr(0, z.size() / 2)});
    }
    return in;
}

std::string read_result_json(const std::string& data) {
    try {
        std::vector<Rows> tabs = spreadsheet::is_xlsx(data)
                                     ? spreadsheet::read_xlsx(data, layout::kSourceTab)
                                     : std::vector<Rows>{spreadsheet::read_csv(data)};
        return "\"tabs\":" + jarr(tabs, rows_json);
    } catch (const spreadsheet::Error& e) {
        return "\"error\":" + jstr(e.what());
    }
}

std::string spreadsheets_json(const std::vector<std::filesystem::path>& fixtures) {
    std::vector<std::string> cases;
    for (const auto& path : fixtures) {
        if (path.extension() != ".xlsx" && path.extension() != ".csv") continue;
        cases.push_back("{\"name\":" + jstr(path.filename().string()) +
                        ",\"file\":" + jstr(path.filename().string()) + "," +
                        read_result_json(read_file(path.string())) + "}");
    }
    for (const auto& [name, data] : generated_inputs()) {
        cases.push_back("{\"name\":" + jstr(name) + ",\"hex\":" + jstr(hex(data)) + "," +
                        read_result_json(data) + "}");
    }
    return jarr(cases, [](const std::string& s) { return s; });
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: lugbulk_golden <out dir>\n";
        return 2;
    }
    const std::string dir = argv[1];
    try {
        layout::load_label_specs(LUGBULK_LABEL_SPECS_PATH);
        std::filesystem::create_directories(dir);

        std::vector<std::filesystem::path> fixtures;
        for (const auto& e : std::filesystem::directory_iterator(LUGBULK_FIXTURES)) {
            if (e.is_regular_file()) fixtures.push_back(e.path());
        }
        std::sort(fixtures.begin(), fixtures.end());
        int cases = 0;
        for (const auto& path : fixtures) {
            std::string data = read_file(path.string());
            std::vector<Rows> tabs;
            if (path.extension() == ".xlsx") {
                try {
                    tabs = spreadsheet::read_xlsx(data, layout::kSourceTab);
                } catch (const spreadsheet::Error& e) {
                    std::cout << "skipped " << path.filename() << " (" << e.what() << ")\n";
                    continue;  // bomb.xlsx: rejected by design
                }
            } else if (path.extension() == ".csv") {
                tabs = {spreadsheet::read_csv(data)};
            } else {
                continue;
            }
            write_file(dir + "/case-" + path.stem().string() + ".json", case_json(tabs));
            ++cases;
        }
        write_file(dir + "/case-generated-too-many-parts.json", case_json(too_many_parts()));
        write_file(dir + "/case-generated-empty.json", case_json({Rows{}}));
        write_file(dir + "/case-generated-no-tabs.json", case_json({}));
        write_file(dir + "/units.json", units_json());
        write_file(dir + "/spreadsheets.json", spreadsheets_json(fixtures));
        if (cases == 0) throw std::runtime_error("no fixtures found in " LUGBULK_FIXTURES);
    } catch (const std::exception& e) {
        std::cerr << "golden: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

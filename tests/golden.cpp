// Writes golden JSON files for the browser port's parity test
// (tests/js/parity.test.mjs): what the C++ pivot/ordering/colors/records
// code makes of each fixture in tests/fixtures (and a few generated cases),
// together with the input rows it read, so the JavaScript in static/js/
// can run on the same rows and must give deep-equal results.
//
//   lugbulk_golden <out dir>
//
// Writes <out dir>/case-<name>.json per sheet and <out dir>/units.json for
// the smaller functions. Every fixture is invented data (no real orders).

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
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
        if (cases == 0) throw std::runtime_error("no fixtures found in " LUGBULK_FIXTURES);
    } catch (const std::exception& e) {
        std::cerr << "golden: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

// Unit tests for the sheet -> labels pipeline. Deliberately dependency-
// free: each CHECK prints the failing expression and the run exits 1.
//
//   cmake --build build && ctest --test-dir build --output-on-failure

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "crow/json.h"

#include "colors.h"
#include "bricklink.h"
#include "image_backdrop.h"
#include "labels_pdf.h"
#include "oauth.h"
#include "rate_limits.h"
#include "db.h"
#include "samples.h"
#include "spreadsheet.h"
#include "zip_writer.h"
#include "ordering.h"
#include "reports.h"
#include "sheet_layout.h"
#include "sheet_pivot.h"

using namespace lugbulk;

namespace {

int g_failures = 0;

#define CHECK(expr)                                                                   \
    do {                                                                              \
        if (!(expr)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #expr "\n"; \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        auto _a = (a);                                                                     \
        auto _b = (b);                                                                     \
        if (!(_a == _b)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b \
                      << "\n  got: " << _a << "\n  want: " << _b << "\n";                  \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

std::vector<std::string> issue_kinds(const PivotResult& r) {
    std::vector<std::string> kinds;
    for (const auto& i : r.issues) kinds.push_back(i.kind);
    return kinds;
}

void test_colors() {
    auto c = colors::resolve("MED. ST-GREY", "");
    CHECK_EQ(c.lego, std::string("Medium Stone Grey"));
    CHECK_EQ(c.bl, std::string("Light Bluish Gray"));
    CHECK(c.mapped);
    CHECK_EQ(colors::resolve("LT. ST. GREY", "").bl, std::string("Light Bluish Gray"));
    CHECK_EQ(colors::resolve("", "Tan").lego, std::string("Brick Yellow"));
    CHECK_EQ(colors::resolve("", "Trans Black").lego, std::string("Transparent Brown"));
    CHECK_EQ(colors::resolve("BR.YEL-GREEN", "Lime").lego, std::string("Bright Yellowish Green"));
    CHECK(!colors::resolve("MYSTERY", "").mapped);
    CHECK(!colors::resolve("", "Mystery").mapped);
    CHECK(colors::is_transparent("TR.L.BLUE", ""));
    CHECK(colors::is_transparent("", "Trans-Clear"));
    CHECK(!colors::is_transparent("WHITE", "White"));
}

void test_estimate_weight() {
    struct Case {
        const char* desc;
        double bricks;
    } cases[] = {
        {"BRICK 1X2", 2},
        {"PLATE 4X8", 32.0 / 3},
        {"FLAT TILE 2X2", 4.0 / 3},
        {"BRICK 1X2X5", 10},
        {"BRICK 1X1X1 2/3 W/2 KNOBS", 5.0 / 3},
        {"ROOF TILE 1X2X2/3", 4.0 / 3},
        {"ROOF TILE 1X2X3/73\xc2\xb0", 6},  // 3 bricks tall, 73° slope
        {"DUPLO BRICK 2X4", 64},
        {"Profile brick 1x2 single gro.", 2},
    };
    for (const auto& c : cases) {
        auto w = ordering::estimate_weight(c.desc);
        CHECK(w.has_value());
        if (w && !near(*w, c.bricks * 0.43)) {
            std::cerr << "estimate_weight(\"" << c.desc << "\") = " << *w << ", want "
                      << c.bricks * 0.43 << "\n";
            ++g_failures;
        }
    }
    CHECK(!ordering::estimate_weight("FROG").has_value());
}

void test_pivot_qty_marker_layout() {
    std::vector<std::vector<std::string>> rows = {
        {"#", "Element ID", "Photo", "Description", "BL Color", "Cost Each", "Total", "Ann Lee",
         "", "Bob Roe", ""},
        {"", "", "", "", "", "", "", "qty", "$$", "qty", "$$"},
        {"1", "4211388", "", "BRICK 1X2", "Light Bluish Gray", "0.05", "", "2,000", "$1", "", ""},
        {"2", "6508677", "", "BRICK 2X4, TRANSPARENT", "Trans-Clear", "0.2", "", "0", "", "25",
         "$5"},
    };
    PivotResult r = pivot_sheet(rows);
    CHECK_EQ(r.records.size(), size_t{2});
    CHECK(r.issues.empty());
    if (r.records.size() == 2) {
        CHECK_EQ(r.records[0].person, std::string("Ann Lee"));
        CHECK_EQ(r.records[0].qty, std::string("2000"));
        CHECK_EQ(r.records[0].lego_color, std::string("Medium Stone Grey"));
        CHECK_EQ(r.records[1].person, std::string("Bob Roe"));
        CHECK_EQ(r.records[1].lego_color, std::string("Transparent"));
    }
}

void test_pivot_name_cost_pair_layout() {
    // Master-sheet layout (made-up values): totals row above the header; each person is
    // (name, running cost); LEGO colors only.
    std::vector<std::vector<std::string>> rows = {
        {"41234", "", "", "", "", "", "", "Ann Lee", ""},
        {"Total Ordered", "Part Number", "Description", "LEGO Color", "BL Color", "Price",
         "Nominated for", "Ann Lee", "$120.50", "Bob Roe", "30.00"},
        {},
        {"1500", "4211407", "PLATE 4X8", "WHITE", "", "0.10", "ZZZ", "100", "10", "x", ""},
    };
    PivotResult r = pivot_sheet(rows);
    CHECK_EQ(r.records.size(), size_t{1});
    if (!r.records.empty()) {
        CHECK_EQ(r.records[0].person, std::string("Ann Lee"));
        CHECK_EQ(r.records[0].lego_color, std::string("White"));
        CHECK_EQ(r.records[0].bl_color, std::string("White"));
    }
    CHECK(issue_kinds(r) == std::vector<std::string>{"bad_qty"});
}

void test_pivot_rejects_bad_element_ids() {
    std::vector<std::vector<std::string>> rows = {
        {"#", "Element ID", "", "Description", "BL Color", "", "", "Ann"},
        {"", "", "", "", "", "", "", "qty"},
        {"1", "../../etc/passwd", "", "BRICK", "Red", "", "", "5"},
        {"", "TOTAL", "", "", "", "", "", ""},  // footer with no qty: skipped silently
    };
    PivotResult r = pivot_sheet(rows);
    CHECK(r.records.empty());
    CHECK(issue_kinds(r) == std::vector<std::string>{"bad_element_id"});
    CHECK(is_valid_element_id("6225242"));
    CHECK(!is_valid_element_id("12"));
    CHECK(!is_valid_element_id("123/456"));
}

void test_pivot_duplicates_unmapped_and_weight() {
    std::vector<std::vector<std::string>> rows = {
        {"#", "Element ID", "", "Description", "LEGO Color", "Weight", "", "Ann", ""},
        {"", "", "", "", "", "", "", "qty", "$$"},
        {"1", "6584805", "", "PROFILE BRICK", "WARM PINK", "0.9 g", "", "5", ""},
        {"2", "6584805", "", "PROFILE BRICK", "WARM PINK", "", "", "5", ""},
        {"3", "1234567", "", "THING", "NEWCOLOR", "heavy", "", "1", ""},
    };
    PivotResult r = pivot_sheet(rows);
    CHECK_EQ(r.records.size(), size_t{3});
    if (!r.records.empty()) {
        CHECK(r.records[0].weight && near(*r.records[0].weight, 0.9));
        CHECK_EQ(r.records[0].bl_color, std::string("Warm Pink"));
    }
    auto kinds = issue_kinds(r);
    std::sort(kinds.begin(), kinds.end());
    CHECK((kinds == std::vector<std::string>{"bad_weight", "duplicate", "unmapped_color"}));
}

LabelRecord rec(const std::string& person, const std::string& id, const std::string& desc,
                const std::string& qty) {
    LabelRecord r;
    r.person = person;
    r.element_id = id;
    r.description = desc;
    r.qty = qty;
    return r;
}

void test_ordering() {
    std::vector<LabelRecord> records = {
        rec("Ann", "1111", "PLATE 1X1", "50"),   rec("Bob", "2222", "BASE PLATE 32X32", "5"),
        rec("Cat", "1111", "PLATE 1X1", "10"),   rec("Dan", "3333", "FROG", "1"),
        rec("Eve", "2222", "BASE PLATE 32X32", "2"),
    };
    auto ordered = ordering::order_records(records, ordering::PartOrder::kHeaviest);
    std::vector<std::string> got;
    for (const auto& r : ordered) {
        got.push_back(r.element_id + ":" + r.person + ":" + std::to_string(r.part_seq) + "/" +
                      std::to_string(r.part_total));
    }
    std::vector<std::string> want = {"2222:Eve:1/2", "2222:Bob:2/2", "1111:Cat:1/2",
                                     "1111:Ann:2/2", "3333:Dan:1/1"};
    CHECK(got == want);

    auto lightest = ordering::summarize_parts(records, ordering::PartOrder::kLightest);
    CHECK_EQ(lightest.front().element_id, std::string("1111"));
    CHECK_EQ(lightest.back().element_id, std::string("3333"));  // unknown size last
    CHECK_EQ(lightest.front().lots, 2);
    CHECK(near(lightest.front().pieces, 60));
    CHECK(!ordering::parse_part_order("bogus").has_value());
}

void test_reports_csv_injection() {
    std::vector<ordering::PartSummary> parts(1);
    parts[0].element_id = "1111";
    parts[0].description = "=HYPERLINK(\"http://x\")";
    parts[0].lots = 1;
    parts[0].pieces = 5;
    std::string csv = reports::parts_csv(parts);
    CHECK(csv.find("\"'=HYPERLINK(\"\"http://x\"\")\"") != std::string::npos);
    CHECK_EQ(reports::weight_text(parts[0]), std::string("size unknown"));
}

void test_backdrop() {
    // A faint gray square on white, as LEGO shoots a white/trans part.
    image_backdrop::RgbImage img{40, 40, std::vector<uint8_t>(40 * 40 * 3, 255)};
    for (int y = 12; y < 28; ++y)
        for (int x = 12; x < 28; ++x)
            for (int c = 0; c < 3; ++c) img.pixels[(y * 40 + x) * 3 + c] = 235;
    auto px = [](const image_backdrop::RgbImage& im, int x, int y) {
        return int{im.pixels[(static_cast<size_t>(y) * im.width + x) * 3]};
    };

    auto white = image_backdrop::backdrop(img, /*trans=*/false, /*light_color=*/true);
    CHECK(white.has_value());
    if (white) {
        CHECK_EQ(white->width, 80);
        CHECK(px(*white, 20, 40) < 230);  // background became the gray tile
        CHECK(px(*white, 40, 40) >= 230);  // the part stayed its own (light) color
        CHECK_EQ(px(*white, 0, 0), 255);   // rounded tile corner stays white
    }
    auto trans = image_backdrop::backdrop(img, true, false);
    CHECK(trans.has_value());
    if (trans) CHECK(px(*trans, 40, 40) < px(*trans, 20, 40));  // glass darkens the tile

    // A dark (clearly visible) part in an ordinary color is left alone.
    for (auto& p : img.pixels) if (p == 235) p = 60;
    CHECK(!image_backdrop::backdrop(img, false, false).has_value());
}

void test_bricklink_catalog() {
    char dir_template[] = "/tmp/lugbulk_bl_XXXXXX";
    std::string dir = mkdtemp(dir_template);
    CHECK(bricklink::load(dir).empty());  // no files
    // BrickLink's downloads are tab-delimited with CRLF; any file names.
    std::ofstream(dir + "/downloaded-1.txt")
        << "Category ID\tCategory Name\tNumber\tName\tAlternate Item Number\tWeight (in Grams)\r\n"
           "\r\n5\tBrick\t3004\tBrick 1 x 2\t3004f1\t0.83\r\n"
           "28\tAnimal\tx223\tFrog\t\t?\r\n";
    CHECK(bricklink::load(dir).empty());  // codes file still missing
    std::ofstream(dir + "/whatever.txt")
        << "Item No\tColor\tCode\r\n3004\tLight Bluish Gray\t4211388\r\nx223\tBlack\t6584302\r\n";
    auto catalog = bricklink::load(dir);
    CHECK_EQ(catalog.size(), size_t{2});
    auto brick = catalog.find("4211388");
    CHECK(brick != catalog.end() && brick->second.part_no == "3004" &&
          brick->second.color == "Light Bluish Gray" && brick->second.weight &&
          near(*brick->second.weight, 0.83));
    auto frog = catalog.find("6584302");
    CHECK(frog != catalog.end() && !frog->second.weight);  // "?" weight

    bricklink::CatalogCache cache(dir);
    CHECK_EQ(cache.get()->size(), size_t{2});
    std::ofstream(dir + "/whatever.txt", std::ios::app) << "3004\tWhite\t300101\r\n";
    CHECK_EQ(cache.get()->size(), size_t{3});  // picks up the changed file
    for (const char* f : {"/downloaded-1.txt", "/whatever.txt"}) std::remove((dir + f).c_str());
    rmdir(dir.c_str());
}

// POST /bricklink/lookup's request parsing and response.
void test_bricklink_lookup() {
    std::string error;
    auto ids = bricklink::parse_lookup_request(R"({"ids":["4211388","6584302","9999999"]})", &error);
    CHECK(ids && ids->size() == 3 && (*ids)[0] == "4211388");
    CHECK(bricklink::parse_lookup_request(R"({"ids":[]})", &error).has_value());
    for (const char* bad : {"", "[]", "{}", R"({"ids":"4211388"})", R"({"ids":[4211388]})",
                            R"({"ids":["42"]})", R"({"ids":["../etc"]})", R"({"ids":["4211388x"]})"}) {
        error.clear();
        if (bricklink::parse_lookup_request(bad, &error) || error.empty()) {
            std::cerr << "lookup request accepted: " << bad << "\n";
            ++g_failures;
        }
    }
    auto many = [](size_t n) {
        std::string body = R"({"ids":[)";
        for (size_t i = 0; i < n; ++i) body += (i ? ",\"" : "\"") + std::to_string(1000000 + i) + "\"";
        return body + "]}";
    };
    CHECK(bricklink::parse_lookup_request(many(bricklink::kMaxLookupIds), &error).has_value());
    CHECK(!bricklink::parse_lookup_request(many(bricklink::kMaxLookupIds + 1), &error));

    bricklink::Catalog catalog;
    catalog["4211388"] = {"3004", "Light Bluish Gray", 1.22};
    catalog["6584302"] = {"x223", "Black \"Pearl\"", std::nullopt};
    CHECK_EQ(bricklink::lookup_json(catalog, {"4211388", "9999999", "6584302", "4211388"}),
             std::string(R"({"4211388":{"part":"3004","color":"Light Bluish Gray","weight":1.22},)"
                         R"("6584302":{"part":"x223","color":"Black \"Pearl\"","weight":null}})"));
    CHECK_EQ(bricklink::lookup_json(catalog, {}), std::string("{}"));
    CHECK_EQ(bricklink::lookup_json(bricklink::Catalog{}, {"4211388"}), std::string("{}"));
}

// GET /img/<name>: which names map to a cache file, and the cache probe.
void test_image_names_and_cache() {
    CHECK(labels_pdf::element_id_from_image_name("6225242.jpg") == std::optional<std::string>("6225242"));
    for (const char* bad : {"abc.jpg", "6225242.png", "6225242", ".jpg", "123.jpg", "../6225242.jpg",
                            "6225242.jpg.jpg", "6225242.JPG", "622%2F242.jpg", "https:x.jpg"}) {
        if (labels_pdf::element_id_from_image_name(bad)) {
            std::cerr << "image name accepted: " << bad << "\n";
            ++g_failures;
        }
    }

    char dir_template[] = "/tmp/lugbulk_img_XXXXXX";
    std::string dir = mkdtemp(dir_template);
    std::string path;
    CHECK(labels_pdf::probe_image_cache("6225242", dir, &path) == labels_pdf::CachedImage::kUnknown);
    CHECK_EQ(path, dir + "/6225242.jpg");
    std::ofstream(dir + "/6225242.jpg", std::ios::binary) << "\xFF\xD8\xFF\xE0jpeg";
    CHECK(labels_pdf::probe_image_cache("6225242", dir, &path) == labels_pdf::CachedImage::kHit);
    std::ofstream(dir + "/300101.jpg", std::ios::binary);  // a fresh cached miss
    CHECK(labels_pdf::probe_image_cache("300101", dir) == labels_pdf::CachedImage::kMiss);
    CHECK(labels_pdf::cached_image_path("300101", "https://invalid.invalid/x.jpg", dir).empty());
    CHECK(labels_pdf::cached_image_path("6225242", "https://invalid.invalid/x.jpg", dir) ==
          dir + "/6225242.jpg");  // a hit never touches the network
    CHECK(labels_pdf::probe_image_cache("../x", dir, &path) == labels_pdf::CachedImage::kMiss);
    CHECK(path.empty());
    for (const char* f : {"/6225242.jpg", "/300101.jpg"}) std::remove((dir + f).c_str());
    rmdir(dir.c_str());
}

void test_placeholder_color_and_catalog_weight() {
    std::vector<std::vector<std::string>> rows = {
        {"#", "Element ID", "", "Description", "BL Color", "", "", "Ann"},
        {"", "", "", "", "", "", "", "qty"},
        {"1", "6584805", "", "PROFILE BRICK", "unknown", "", "", "5"},
    };
    PivotResult r = pivot_sheet(rows);
    CHECK_EQ(r.records.size(), size_t{1});
    CHECK(r.records.empty() || r.records[0].bl_color.empty());
    CHECK(r.issues.size() == 1 && r.issues[0].kind == "missing_color" &&
          r.issues[0].element_id == "6584805");

    std::vector<LabelRecord> records = {rec("A", "1111", "BRICK 1X1", "1"),
                                        rec("B", "2222", "BRICK 1X1", "1")};
    records[0].weight = 9.0;
    records[0].catalog_weight = 1.0;
    records[1].catalog_weight = 2.5;
    auto parts = ordering::summarize_parts(records, ordering::PartOrder::kSheet);
    CHECK_EQ(parts[0].weight_source, std::string("sheet"));
    CHECK_EQ(parts[1].weight_source, std::string("bricklink"));
    CHECK(parts[1].weight && near(*parts[1].weight, 2.5));
}

void test_labels_pdf_every_spec() {
    // Pre-seed the image cache with "cached miss" markers so rendering
    // never touches the network.
    char dir_template[] = "/tmp/lugbulk_tests_XXXXXX";
    std::string dir = mkdtemp(dir_template);
    std::vector<LabelRecord> records = {rec("Ann Lee", "4211388", "BRICK 1X2", "25"),
                                        rec("Bob Roe", "6508677", "BRICK 2X4, TRANSPARENT", "100")};
    for (auto& r : records) {
        r.image_url = layout::image_url_for(r.element_id);
        std::ofstream(dir + "/" + r.element_id + ".jpg");
    }
    records = ordering::order_records(records, ordering::PartOrder::kHeaviest);
    for (const auto& spec : layout::label_specs()) {
        auto pdf = labels_pdf::build_labels_pdf(records, dir, spec);
        CHECK(pdf.size() > 500);
        CHECK(std::string(pdf.begin(), pdf.begin() + 5) == "%PDF-");
    }
    for (auto& r : records) std::remove((dir + "/" + r.element_id + ".jpg").c_str());
    rmdir(dir.c_str());
}

}  // namespace

void test_label_specs() {
    CHECK(layout::label_specs().size() > 40);
    CHECK_EQ(layout::default_label_spec().id, std::string("avery5162"));
    CHECK(layout::find_label_spec("8162") == &layout::default_label_spec());
    CHECK(layout::find_label_spec("Avery 8162") == &layout::default_label_spec());
    const auto* dymo = layout::find_label_spec("dymo30857");
    CHECK(dymo && dymo->page == "roll" && dymo->label_width_mm > dymo->label_height_mm);
    const auto* a4 = layout::find_label_spec("L7163");
    CHECK(a4 && a4->page == "A4");
    CHECK(layout::find_label_spec("bogus") == nullptr);

    // GET /label-specs.json: every loaded stock, every field.
    auto json = crow::json::load(layout::label_specs_json());
    CHECK(json && json["specs"].size() == layout::label_specs().size());
    if (json && json["specs"].size() == layout::label_specs().size()) {
        CHECK_EQ(std::string(json["default"].s()), std::string("avery5162"));
        CHECK(std::string(json["source"].s()).find("gLabels") != std::string::npos);
        for (size_t i = 0; i < layout::label_specs().size(); ++i) {
            const auto& s = layout::label_specs()[i];
            const auto& j = json["specs"][i];
            CHECK_EQ(std::string(j["id"].s()), s.id);
            CHECK_EQ(std::string(j["page"].s()), s.page);
            CHECK_EQ(std::string(j["display_name"].s()), s.display_name());
            CHECK_EQ(j["equivalents"].size(), s.equivalents.size());
            CHECK_EQ(j["columns"].i(), int64_t{s.columns});
            CHECK_EQ(j["per_sheet"].i(), int64_t{s.per_sheet()});
            CHECK(near(j["label_width_mm"].d(), s.label_width_mm));
            CHECK(near(j["top_margin_mm"].d(), s.top_margin_mm));
            CHECK(near(j["column_gap_mm"].d(), s.column_gap_mm));
        }
    }
    // Every stock's grid fits on its page.
    for (const auto& s : layout::label_specs()) {
        double right = s.left_margin_mm + s.columns * s.label_width_mm +
                       (s.columns - 1) * s.column_gap_mm;
        double bottom = s.top_margin_mm + s.rows * s.label_height_mm + (s.rows - 1) * s.row_gap_mm;
        if (right > s.sheet_width_mm + 0.5 || bottom > s.sheet_height_mm + 0.5) {
            std::cerr << s.id << " overflows its page\n";
            ++g_failures;
        }
    }
}

void test_rate_limits() {
    limits::RateLimiter rl(3, 1.0 / 60);  // 3 at once, then one a minute
    CHECK(!rl.take("a") && !rl.take("a") && !rl.take("a"));
    auto retry = rl.take("a");
    CHECK(retry.has_value() && *retry > 0 && *retry <= 60);
    CHECK(!rl.take("b"));  // keys are independent

    limits::JobGate gate(2);
    {
        auto a = gate.enter(1);
        CHECK(a.ticket.has_value());
        auto again = gate.enter(1);
        CHECK(!again.ticket && again.refusal == limits::JobGate::Refusal::kUserBusy);
        auto b = gate.enter(2);
        CHECK(b.ticket.has_value());
        auto c = gate.enter(3);
        CHECK(!c.ticket && c.refusal == limits::JobGate::Refusal::kServerBusy);
    }
    CHECK(gate.enter(3).ticket.has_value());  // tickets released on scope exit

    limits::Allowlist open("");
    CHECK(open.allows("anyone@example.com"));
    limits::Allowlist list(" Ann@Example.com, @lug.org ");
    CHECK(list.allows("ann@example.com"));
    CHECK(list.allows("BOB@lug.org"));
    CHECK(!list.allows("bob@example.com"));
    CHECK(!list.allows("ann@example.com.evil.com"));
    CHECK(!list.allows("@lug.org"));
}

void test_label_options_and_extras() {
    using labels_pdf::LabelOptions;
    using labels_pdf::LabelPart;
    LabelOptions defaults;
    CHECK(defaults.show(LabelPart::kPhoto) && !defaults.show(LabelPart::kQr));
    CHECK_EQ(defaults.hidden_csv(), std::string("qr"));
    auto o = LabelOptions::parse("photo, bl_color", "qr");
    CHECK(o && !o->show(LabelPart::kPhoto) && !o->show(LabelPart::kBlColor) && o->show(LabelPart::kQr));
    std::string err;
    CHECK(!LabelOptions::parse("bogus", "", &err) && err.find("bogus") != std::string::npos);
    auto all = LabelOptions::from_hidden("");
    CHECK(all && all->show(LabelPart::kQr) && all->hidden_csv().empty());
    auto h = LabelOptions::from_hidden("qr,name");
    CHECK(h && !h->show(LabelPart::kQr) && !h->show(LabelPart::kName));
    CHECK_EQ(h->hidden_csv(), std::string("name,qr"));

    auto rgb = colors::swatch_rgb("", "Light Bluish Gray");
    CHECK(rgb && std::fabs((*rgb)[0] - 0xA0 / 255.0) < 1e-9);
    CHECK(!colors::swatch_rgb("MYSTERY", ""));
    CHECK_EQ(labels_pdf::bricklink_url("6225242"),
             std::string("https://www.bricklink.com/v2/search.page?q=6225242"));

    auto records = samples::sample_records();
    CHECK(records.size() == 9 && records.front().part_total > 0);

    // Every combination renders, from no parts to everything (incl. QR),
    // with photos as cached misses so nothing touches the network.
    char dir_template[] = "/tmp/lugbulk_opts_XXXXXX";
    std::string dir = mkdtemp(dir_template);
    for (const auto& r : records) std::ofstream(dir + "/" + r.element_id + ".jpg");
    const char* designs[] = {"", "qr", "photo,element_id,qty,lego_color,bl_color,description,name,count",
                             "photo,swatch", "name,count"};
    for (const char* hide : designs) {
        for (const char* stock : {"avery5160", "avery5162", "dymo30857"}) {
            auto pdf = labels_pdf::build_labels_pdf(records, dir, *layout::find_label_spec(stock),
                                                    *LabelOptions::from_hidden(hide), 1);
            CHECK(pdf.size() > 500);
        }
    }
    auto page = labels_pdf::build_test_page(*layout::find_label_spec("avery5160"));
    CHECK(std::string(page.begin(), page.begin() + 5) == "%PDF-");
    auto checklist = reports::checklist_pdf(records);
    CHECK(checklist.size() > 1000);
    for (const auto& r : records) std::remove((dir + "/" + r.element_id + ".jpg").c_str());
    rmdir(dir.c_str());
}

void test_design_storage() {
    char dir_template[] = "/tmp/lugbulk_db_XXXXXX";
    std::string dir = mkdtemp(dir_template);
    {
        Db db(dir + "/t.sqlite3", LUGBULK_SCHEMA_PATH);
        std::vector<uint8_t> token{1, 2, 3};
        User u = db.upsert_user("sub-1", "ann@example.com", &token);
        CHECK(!db.get_design("sheet1"));
        db.put_design("sheet1", Design{"avery5160", "lightest", "qr,photo"}, u.id);
        db.put_design("sheet1", Design{"dymo30857", "sheet", "name"}, u.id);
        auto d = db.get_design("sheet1");
        CHECK(d && d->label_spec == "dymo30857" && d->part_order == "sheet" && d->hidden_parts == "name");
    }
    std::remove((dir + "/t.sqlite3").c_str());
    rmdir(dir.c_str());
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void test_spreadsheet_uploads() {
    // The CLI's sample order sheet (2026 master layout, made-up names).
    std::string xlsx = read_file(LUGBULK_FIXTURES "/sample_order.xlsx");
    CHECK(spreadsheet::is_xlsx(xlsx));
    auto sheets = spreadsheet::read_xlsx(xlsx, layout::kSourceTab);  // tab is "OrderHere"
    CHECK_EQ(sheets.size(), size_t{1});
    if (!sheets.empty()) {
        PivotResult r = pivot_sheet(sheets[0]);
        CHECK_EQ(r.records.size(), size_t{6});
        CHECK(r.issues.empty());
        bool found = false;
        for (const auto& rec : r.records) {
            if (rec.element_id == "4211388" && rec.person == "Bob Roe") {
                found = rec.qty == "25" && rec.lego_color == "Medium Stone Grey";
            }
        }
        CHECK(found);
    }

    std::string csv = "\xEF\xBB\xBF#,Element ID,Photo,Description,BL Color,Cost,Total,Ann Lee,\r\n"
                      ",,,,,,,qty,$$\r\n"
                      "1,4211388,,\"BRICK 1X2, GREY\",Light Bluish Gray,0.05,,\"2,000\",$1\r\n";
    CHECK(!spreadsheet::is_xlsx(csv));
    auto rows = spreadsheet::read_csv(csv);
    CHECK_EQ(rows.size(), size_t{3});
    PivotResult r = pivot_sheet(rows);
    CHECK(r.records.size() == 1 && r.records[0].qty == "2000" &&
          r.records[0].description == "BRICK 1X2, GREY");

    // A zip bomb (70 MB of spaces, ~70 KB compressed) is refused, not unpacked.
    bool refused = false;
    try {
        spreadsheet::read_xlsx(read_file(LUGBULK_FIXTURES "/bomb.xlsx"), layout::kSourceTab);
    } catch (const spreadsheet::Error& e) {
        refused = std::string(e.what()).find("64 MB") != std::string::npos;
    }
    CHECK(refused);

    bool garbage = false;
    try {
        spreadsheet::read_xlsx("PK\x03\x04 not really a zip", layout::kSourceTab);
    } catch (const spreadsheet::Error&) {
        garbage = true;
    }
    CHECK(garbage);
}

void test_zip_writer() {
    std::string zip = zip_writer::zip({{"a labels.pdf", "%PDF-1.4 hello"}, {"b.csv", "x,y\r\n"}});
    CHECK(zip.rfind("PK\x03\x04", 0) == 0);
    // End of central directory: 2 entries.
    size_t eocd = zip.rfind("PK\x05\x06");
    CHECK(eocd != std::string::npos && eocd + 22 == zip.size());
    CHECK(static_cast<unsigned char>(zip[eocd + 10]) == 2);
    // The stored bytes and names are there, and a real unzip agrees.
    CHECK(zip.find("%PDF-1.4 hello") != std::string::npos && zip.find("b.csv") != std::string::npos);
    char path[] = "/tmp/lugbulk_zip_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(write(fd, zip.data(), zip.size()) == static_cast<ssize_t>(zip.size()));
        close(fd);
        std::string cmd = std::string("unzip -tq ") + path + " >/dev/null 2>&1";
        if (std::system("command -v unzip >/dev/null 2>&1") == 0) CHECK(std::system(cmd.c_str()) == 0);
        std::remove(path);
    }
}

void test_oauth_scopes() {
    const std::string granted = "openid https://www.googleapis.com/auth/drive.file "
                                "https://www.googleapis.com/auth/userinfo.email";
    CHECK(oauth::has_scope(granted, oauth::kDriveFileScope));
    CHECK(oauth::has_scope(granted, "openid"));
    CHECK(!oauth::has_scope(granted, "https://www.googleapis.com/auth/drive"));
    CHECK(!oauth::has_scope("", oauth::kDriveFileScope));
    // A sign-in from before the Picker: the old scopes only.
    CHECK(!oauth::has_scope("openid https://www.googleapis.com/auth/spreadsheets.readonly "
                            "https://www.googleapis.com/auth/drive.metadata.readonly",
                            oauth::kDriveFileScope));
}

int main() {
    layout::load_label_specs(LUGBULK_LABEL_SPECS_PATH);
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"colors", test_colors},
        {"estimate_weight", test_estimate_weight},
        {"pivot_qty_marker_layout", test_pivot_qty_marker_layout},
        {"pivot_name_cost_pair_layout", test_pivot_name_cost_pair_layout},
        {"pivot_rejects_bad_element_ids", test_pivot_rejects_bad_element_ids},
        {"pivot_duplicates_unmapped_and_weight", test_pivot_duplicates_unmapped_and_weight},
        {"ordering", test_ordering},
        {"reports_csv_injection", test_reports_csv_injection},
        {"backdrop", test_backdrop},
        {"bricklink_catalog", test_bricklink_catalog},
        {"bricklink_lookup", test_bricklink_lookup},
        {"image_names_and_cache", test_image_names_and_cache},
        {"placeholder_color_and_catalog_weight", test_placeholder_color_and_catalog_weight},
        {"label_specs", test_label_specs},
        {"spreadsheet_uploads", test_spreadsheet_uploads},
        {"zip_writer", test_zip_writer},
        {"oauth_scopes", test_oauth_scopes},
        {"rate_limits", test_rate_limits},
        {"label_options_and_extras", test_label_options_and_extras},
        {"design_storage", test_design_storage},
        {"labels_pdf_every_spec", test_labels_pdf_every_spec},
    };
    for (const auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::cout << (g_failures == before ? "PASS " : "FAIL ") << name << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}

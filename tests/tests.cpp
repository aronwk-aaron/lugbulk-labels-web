// Unit tests for the sheet -> labels pipeline. Deliberately dependency-
// free: each CHECK prints the failing expression and the run exits 1.
//
//   cmake --build build && ctest --test-dir build --output-on-failure

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "colors.h"
#include "bricklink.h"
#include "image_backdrop.h"
#include "labels_pdf.h"
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
    // 2026 master sheet: totals row above the header; each person is
    // (name, running cost); LEGO colors only.
    std::vector<std::vector<std::string>> rows = {
        {"83961", "", "", "", "", "", "", "Ann Lee", ""},
        {"Total Ordered", "Part Number", "Description", "LEGO Color", "BL Color", "Price",
         "Nominated for", "Ann Lee", "$232.45", "Bob Roe", "44.25"},
        {},
        {"2650", "4211407", "PLATE 4X8", "WHITE", "", "0.13", "MILS", "100", "13", "x", ""},
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

void test_bricklink_oauth_signature() {
    // Twitter's published OAuth 1.0a signing walkthrough.
    bricklink::Credentials creds{"xvz1evFS4wEEPTGEFPHBog", "kAcSOqF21Fu85e7zjz7ZN2U4ZRhfV3WpwPAoE3Z7kBw",
                                 "370773112-GmHxMAgYyLbNEtIKZeRNFsMKPR9EyMZeS9weJAEb",
                                 "LswwdoUaIvS8ltyTt5jkRh4J50vUPVVHtR2YPi5kE"};
    std::string header = bricklink::oauth_header(
        "POST", "https://api.twitter.com/1.1/statuses/update.json", creds,
        {{"include_entities", "true"},
         {"status", "Hello Ladies + Gentlemen, a signed OAuth request!"}},
        "kYjzVBB8Y0ZFabxSWbWovY3uYSQ2pTgmZeNu2VS4cg", "1318622958");
    CHECK(header.find("oauth_signature=\"hCtSmYh%2BiHYCEqBWrE7C7hYmtUk%3D\"") != std::string::npos);
    CHECK((!bricklink::Credentials{"a", "b", "c", ""}.complete()));
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
        {"bricklink_oauth_signature", test_bricklink_oauth_signature},
        {"placeholder_color_and_catalog_weight", test_placeholder_color_and_catalog_weight},
        {"label_specs", test_label_specs},
        {"labels_pdf_every_spec", test_labels_pdf_every_spec},
    };
    for (const auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::cout << (g_failures == before ? "PASS " : "FAIL ") << name << "\n";
    }
    return g_failures == 0 ? 0 : 1;
}

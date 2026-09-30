#include "colors.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <initializer_list>
#include <unordered_map>
#include <utility>

namespace lugbulk::colors {

namespace {

struct Entry {
    const char* lego;
    const char* bl;
    std::initializer_list<const char*> aliases;  // LEGO spellings seen on LUGBulk lists
};

// Keep in sync with lugbulk-label's colors.py (same entries, same order).
// "LT. ST. GREY" is 2026's spelling of Medium Stone Grey (e.g. element
// 6225242), not LEGO's separate Light Stone Grey.
const Entry kColors[] = {
    {"White", "White", {"WHITE"}},
    {"Black", "Black", {"BLACK"}},
    {"Bright Red", "Red", {"BR.RED", "BR. RED"}},
    {"Bright Blue", "Blue", {"BR.BLUE", "BR. BLUE"}},
    {"Bright Yellow", "Yellow", {"BR.YEL", "BR. YEL", "BR.YELLOW"}},
    {"Bright Green", "Bright Green", {"BR.GREEN", "BR. GREEN"}},
    {"Dark Green", "Green", {"DK.GREEN", "DK. GREEN"}},
    {"Earth Green", "Dark Green", {"EARTH GREEN"}},
    {"Earth Blue", "Dark Blue", {"EARTH BLUE"}},
    {"Medium Stone Grey", "Light Bluish Gray", {"MED. ST-GREY", "MED.ST-GREY", "MED. ST. GREY", "M. ST. GREY", "LT. ST. GREY", "LT.ST.GREY"}},
    {"Dark Stone Grey", "Dark Bluish Gray", {"DK. ST. GREY", "DK.ST.GREY", "DK. ST-GREY"}},
    {"Brick Yellow", "Tan", {"BRICK-YEL", "BRICK YEL", "BRICK-YELLOW"}},
    {"Sand Yellow", "Dark Tan", {"SAND YELLOW"}},
    {"Reddish Brown", "Reddish Brown", {"RED. BROWN", "RED.BROWN"}},
    {"Dark Brown", "Dark Brown", {"DK. BROWN", "DK.BROWN"}},
    {"New Dark Red", "Dark Red", {"NEW DARK RED"}},
    {"Sand Green", "Sand Green", {"SAND GREEN"}},
    {"Sand Blue", "Sand Blue", {"SAND BLUE"}},
    {"Olive Green", "Olive Green", {"OLIVE GREEN"}},
    {"Nougat", "Nougat", {"NOUGAT"}},
    {"Medium Nougat", "Medium Nougat", {"M. NOUGAT", "MED. NOUGAT", "M.NOUGAT"}},
    {"Light Nougat", "Light Nougat", {"L.NOUGAT", "L. NOUGAT", "LGH. NOUGAT"}},
    {"Dark Orange", "Dark Orange", {"DK.ORA", "DK. ORA", "DK.ORANGE"}},
    {"Bright Orange", "Orange", {"BR.ORANGE", "BR. ORANGE", "BR.ORA"}},
    {"Reddish Orange", "Reddish Orange", {"RED. ORANGE", "RED.ORANGE"}},
    {"Flame Yellowish Orange", "Bright Light Orange", {"FL. YELL-ORA", "FL.YELL-ORA"}},
    {"Bright Yellowish Green", "Lime", {"BR.YEL-GREEN", "BR. YEL-GREEN"}},
    {"Bright Bluish Green", "Dark Turquoise", {"BR.BLUEGREEN", "BR. BLUEGREEN"}},
    {"Aqua", "Light Aqua", {"AQUA"}},
    {"Lavender", "Lavender", {"LAVENDER"}},
    {"Medium Lavender", "Medium Lavender", {"M. LAVENDER", "MED. LAVENDER"}},
    {"Medium Lilac", "Dark Purple", {"MEDIUM LILAC", "M. LILAC"}},
    {"Bright Reddish Violet", "Magenta", {"BR.RED-VIOLET", "BR.RED.VIOLET"}},
    {"Bright Purple", "Dark Pink", {"BR.PURPLE", "BR. PURPLE"}},
    {"Light Purple", "Bright Pink", {"LGH. PURPLE", "LGH.PURPLE"}},
    {"Medium Blue", "Medium Blue", {"MEDIUM BLUE", "M. BLUE"}},
    {"Medium Azur", "Medium Azure", {"MEDIUM AZUR", "MED. AZUR"}},
    {"Dark Azur", "Dark Azure", {"DARK AZUR", "DK. AZUR"}},
    {"Light Royal Blue", "Bright Light Blue", {"LT.ROY.BLUE", "LGH. ROYAL BLUE"}},
    {"Cool Yellow", "Bright Light Yellow", {"COOL YELLOW"}},
    {"Vibrant Coral", "Coral", {"VIBRANT CORAL"}},
    {"Warm Pink", "Warm Pink", {"WARM PINK"}},
    {"Spring Yellowish Green", "Yellowish Green", {"SPR. YEL-GREEN"}},
    {"Silver Metallic", "Flat Silver", {"SILVER MET.", "SILVER MET"}},
    {"Titanium Metallic", "Pearl Dark Gray", {"TITAN. MET.", "TITANIUM MET."}},
    {"Warm Gold", "Pearl Gold", {"WARM GOLD"}},
    {"White Glow", "Glow In Dark White", {"WHITE GLOW"}},
    {"Transparent", "Trans-Clear", {"TR.", "TR", "TRANSPARENT"}},
    {"Transparent Light Blue", "Trans-Light Blue", {"TR.L.BLUE", "TR. L. BLUE"}},
    {"Transparent Blue", "Trans-Dark Blue", {"TR.BLUE", "TR. BLUE"}},
    {"Transparent Brown", "Trans-Black", {"TR.BROWN", "TR. BROWN"}},
    {"Transparent Red", "Trans-Red", {"TR.RED", "TR. RED"}},
    {"Transparent Green", "Trans-Green", {"TR.GREEN", "TR. GREEN"}},
    {"Transparent Yellow", "Trans-Yellow", {"TR.YEL", "TR. YELLOW", "TR.YELLOW"}},
    {"Transparent Bright Orange", "Trans-Orange", {"TR.BR.ORANGE"}},
    {"Transparent Fluorescent Reddish Orange", "Trans-Neon Orange", {"TR.FL.RED-ORA"}},
    {"Transparent Fluorescent Green", "Trans-Neon Green", {"TR.FL.GREEN"}},
};

// Approximate sRGB per LEGO color (BrickLink/Rebrickable swatches), for the
// label color swatch. Keep in sync with lugbulk-label's colors.RGB.
const std::pair<const char*, uint32_t> kRgb[] = {
    {"White", 0xFFFFFF},
    {"Black", 0x1B1B1B},
    {"Bright Red", 0xC91A09},
    {"Bright Blue", 0x0055BF},
    {"Bright Yellow", 0xF2CD37},
    {"Bright Green", 0x4B9F4A},
    {"Dark Green", 0x237841},
    {"Earth Green", 0x184632},
    {"Earth Blue", 0x0A3463},
    {"Medium Stone Grey", 0xA0A5A9},
    {"Dark Stone Grey", 0x6C6E68},
    {"Brick Yellow", 0xE4CD9E},
    {"Sand Yellow", 0x958A73},
    {"Reddish Brown", 0x582A12},
    {"Dark Brown", 0x352100},
    {"New Dark Red", 0x720E0F},
    {"Sand Green", 0xA0BCAC},
    {"Sand Blue", 0x6074A1},
    {"Olive Green", 0x9B9A5A},
    {"Nougat", 0xD09168},
    {"Medium Nougat", 0xAA7D55},
    {"Light Nougat", 0xF6D7B3},
    {"Dark Orange", 0xA95500},
    {"Bright Orange", 0xFE8A18},
    {"Reddish Orange", 0xCA4C0B},
    {"Flame Yellowish Orange", 0xF8BB3D},
    {"Bright Yellowish Green", 0xBBE90B},
    {"Bright Bluish Green", 0x008F9B},
    {"Aqua", 0xADC3C0},
    {"Lavender", 0xE1D5ED},
    {"Medium Lavender", 0xAC78BA},
    {"Medium Lilac", 0x3F3691},
    {"Bright Reddish Violet", 0x923978},
    {"Bright Purple", 0xC870A0},
    {"Light Purple", 0xE4ADC8},
    {"Medium Blue", 0x5A93DB},
    {"Medium Azur", 0x36AEBF},
    {"Dark Azur", 0x078BC9},
    {"Light Royal Blue", 0x9FC3E9},
    {"Cool Yellow", 0xFFF03A},
    {"Vibrant Coral", 0xFF698F},
    {"Warm Pink", 0xF7A1B8},
    {"Spring Yellowish Green", 0xDFEEA5},
    {"Silver Metallic", 0x898788},
    {"Titanium Metallic", 0x575857},
    {"Warm Gold", 0xAA7F2E},
    {"White Glow", 0xD9E4A7},
    {"Transparent", 0xEEEEEE},
    {"Transparent Light Blue", 0xAEEFEC},
    {"Transparent Blue", 0x0020A0},
    {"Transparent Brown", 0x635F52},
    {"Transparent Red", 0xC91A09},
    {"Transparent Green", 0x237841},
    {"Transparent Yellow", 0xF5CD2F},
    {"Transparent Bright Orange", 0xF08F1C},
    {"Transparent Fluorescent Reddish Orange", 0xFF800D},
    {"Transparent Fluorescent Green", 0xF8F184},
};

std::string key(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

using Table = std::unordered_map<std::string, std::pair<std::string, std::string>>;

const Table& by_lego() {
    static const Table table = [] {
        Table t;
        for (const auto& e : kColors) {
            t.emplace(key(e.lego), std::make_pair(e.lego, e.bl));
            for (const char* alias : e.aliases) t.emplace(key(alias), std::make_pair(e.lego, e.bl));
        }
        return t;
    }();
    return table;
}

const Table& by_bl() {
    static const Table table = [] {
        Table t;
        for (const auto& e : kColors) t.emplace(key(e.bl), std::make_pair(e.lego, e.bl));
        return t;
    }();
    return table;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

}  // namespace

Resolved resolve(const std::string& lego_in, const std::string& bl_in) {
    std::string lego = trim(lego_in), bl = trim(bl_in);
    if (!lego.empty()) {
        auto it = by_lego().find(key(lego));
        if (it != by_lego().end()) {
            return {it->second.first, bl.empty() ? it->second.second : bl, true};
        }
        return {lego, bl, !bl.empty()};
    }
    if (!bl.empty()) {
        // A LEGO name typed into the BL column still identifies the color.
        for (const Table* table : {&by_bl(), &by_lego()}) {
            auto it = table->find(key(bl));
            if (it != table->end()) return {it->second.first, bl, true};
        }
        return {"", bl, false};
    }
    return {"", "", true};  // nothing to map; missing_color is reported separately
}

bool is_transparent(const std::string& lego, const std::string& bl) {
    // Every trans color starts "TR"/"Trans" in both naming schemes, and no
    // opaque color does.
    return key(lego).rfind("TR", 0) == 0 || key(bl).rfind("TR", 0) == 0;
}

std::optional<std::array<double, 3>> swatch_rgb(const std::string& lego, const std::string& bl) {
    const std::pair<const Table*, const std::string*> lookups[] = {
        {&by_lego(), &lego}, {&by_bl(), &bl}, {&by_lego(), &bl}};
    for (const auto& [table, name] : lookups) {
        auto it = table->find(key(*name));
        if (it == table->end()) continue;
        for (const auto& [lego_name, rgb] : kRgb) {
            if (it->second.first == lego_name) {
                return std::array<double, 3>{((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
                                             (rgb & 0xff) / 255.0};
            }
        }
    }
    return std::nullopt;
}

bool is_light(const std::string& lego, const std::string& bl) {
    return key(lego).find("WHITE") != std::string::npos ||
           key(bl).find("WHITE") != std::string::npos;
}

}  // namespace lugbulk::colors

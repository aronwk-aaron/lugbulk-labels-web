#include "colors.h"

#include <cctype>
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

bool is_light(const std::string& lego, const std::string& bl) {
    return key(lego).find("WHITE") != std::string::npos ||
           key(bl).find("WHITE") != std::string::npos;
}

}  // namespace lugbulk::colors

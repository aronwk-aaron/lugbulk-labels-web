// LEGO <-> BrickLink color name lookup — port of lugbulk-label's colors.py.
//
// LUGBulk order sheets name colors two ways, and a given sheet may only
// fill in one: LEGO's own (usually abbreviated, "MED. ST-GREY") names, or
// BrickLink's ("Light Bluish Gray"). Labels show both, so the missing one
// is looked up here. Lookups ignore punctuation, spacing and case.
#pragma once

#include <array>
#include <optional>
#include <string>

namespace lugbulk::colors {

struct Resolved {
    std::string lego;  // LEGO's full name when recognized, else the sheet's text
    std::string bl;    // BrickLink name
    bool mapped;       // false if one side was missing and couldn't be looked up
};

// A recognized LEGO abbreviation is expanded to LEGO's full name; a
// BrickLink name the sheet provides is kept as-is.
Resolved resolve(const std::string& lego, const std::string& bl);

// True for trans colors, whose parts photograph as a faint outline on
// LEGO's white-background product shots.
bool is_transparent(const std::string& lego, const std::string& bl);

// True for white-family colors (White, Glow In Dark White, ...), which are
// hard to see on LEGO's white-background product shots.
bool is_light(const std::string& lego, const std::string& bl);

// (r, g, b) in 0..1 for a label's color swatch, or nullopt if unknown.
std::optional<std::array<double, 3>> swatch_rgb(const std::string& lego, const std::string& bl);

}  // namespace lugbulk::colors

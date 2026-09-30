// Makes light parts (trans, white) visible on a printed label — port of
// lugbulk-label's render_labels._backdrop_image.
//
// LEGO's product shots are on pure white, so trans and white parts all but
// disappear once printed. They're set on a light gray rounded tile instead:
// the tile is the photo multiplied onto gray (white background -> tile
// gray; the part's shading and edges carry through), which alone is how a
// trans part is shown — it reads like glass in front of the tile. A white
// part is also cut out and put back on top at full brightness so it still
// looks white.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lugbulk::image_backdrop {

struct RgbImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // row-major, 3 bytes per pixel
};

// Decodes a baseline/progressive JPEG file to 8-bit RGB. nullopt if the
// file can't be read or decoded.
std::optional<RgbImage> decode_jpeg(const std::string& path);

// The tile version of `photo` (at 2x its size), or nullopt when the photo
// is fine as-is: the part isn't trans, isn't a white-family color, and
// isn't faint in the photo either.
std::optional<RgbImage> backdrop(const RgbImage& photo, bool trans, bool light_color);

}  // namespace lugbulk::image_backdrop

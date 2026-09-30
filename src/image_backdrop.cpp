#include "image_backdrop.h"

#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <deque>
#include <memory>

namespace lugbulk::image_backdrop {

namespace {

// Keep in sync with render_labels.py.
constexpr std::array<uint8_t, 3> kTile{218, 220, 224};
constexpr int kBgTolerance = 4;     // within this of pure white (every channel) = background
constexpr double kHoleMin = 0.02;   // enclosed white area (fraction of image) = background
constexpr double kSpeckMin = 0.15;  // blobs smaller than this fraction of the largest are dropped
constexpr int kScale = 2;
constexpr double kTransGain = 1.6;
constexpr int kFaintPartMin = 150;

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

void on_jpeg_error(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    std::longjmp(err->jump, 1);
}

// Single-channel 8-bit image.
struct Gray {
    int w = 0, h = 0;
    std::vector<uint8_t> v;
    uint8_t& at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
    uint8_t at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
};

// Square min (erode) or max (dilate) filter of the given radius, edges clamped.
Gray rank_filter(const Gray& in, bool dilate, int radius) {
    Gray out{in.w, in.h, std::vector<uint8_t>(in.v.size())};
    for (int y = 0; y < in.h; ++y) {
        for (int x = 0; x < in.w; ++x) {
            uint8_t m = dilate ? 0 : 255;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    uint8_t n = in.at(std::clamp(x + dx, 0, in.w - 1), std::clamp(y + dy, 0, in.h - 1));
                    m = dilate ? std::max(m, n) : std::min(m, n);
                }
            }
            out.at(x, y) = m;
        }
    }
    return out;
}

Gray gaussian_blur(const Gray& in, double sigma) {
    int r = std::max(1, static_cast<int>(std::ceil(sigma * 3)));
    std::vector<double> k(2 * r + 1);
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[i + r] = std::exp(-(i * i) / (2 * sigma * sigma));
    for (double& x : k) x /= sum;

    std::vector<double> tmp(in.v.size());
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            double acc = 0;
            for (int i = -r; i <= r; ++i) acc += k[i + r] * in.at(std::clamp(x + i, 0, in.w - 1), y);
            tmp[static_cast<size_t>(y) * in.w + x] = acc;
        }
    Gray out{in.w, in.h, std::vector<uint8_t>(in.v.size())};
    for (int y = 0; y < in.h; ++y)
        for (int x = 0; x < in.w; ++x) {
            double acc = 0;
            for (int i = -r; i <= r; ++i)
                acc += k[i + r] * tmp[static_cast<size_t>(std::clamp(y + i, 0, in.h - 1)) * in.w + x];
            out.at(x, y) = static_cast<uint8_t>(std::clamp(std::lround(acc), 0L, 255L));
        }
    return out;
}

// Bilinear resample of `channels`-channel pixels to `scale`x the size.
std::vector<uint8_t> upscale(const std::vector<uint8_t>& src, int w, int h, int channels, int scale) {
    int W = w * scale, H = h * scale;
    std::vector<uint8_t> out(static_cast<size_t>(W) * H * channels);
    for (int y = 0; y < H; ++y) {
        double sy = std::clamp((y + 0.5) / scale - 0.5, 0.0, h - 1.0);
        int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, h - 1);
        double fy = sy - y0;
        for (int x = 0; x < W; ++x) {
            double sx = std::clamp((x + 0.5) / scale - 0.5, 0.0, w - 1.0);
            int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, w - 1);
            double fx = sx - x0;
            for (int c = 0; c < channels; ++c) {
                auto p = [&](int xx, int yy) {
                    return src[(static_cast<size_t>(yy) * w + xx) * channels + c];
                };
                double v = (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) +
                           (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
                out[(static_cast<size_t>(y) * W + x) * channels + c] = static_cast<uint8_t>(std::lround(v));
            }
        }
    }
    return out;
}

// Connected regions (4-neighbour) of pixels where `in_region` is true.
template <typename Pred>
std::vector<std::vector<int>> regions(int w, int h, Pred in_region) {
    std::vector<uint8_t> seen(static_cast<size_t>(w) * h, 0);
    std::vector<std::vector<int>> out;
    for (int start = 0; start < w * h; ++start) {
        if (seen[start] || !in_region(start)) continue;
        std::vector<int> pixels;
        std::deque<int> queue{start};
        seen[start] = 1;
        while (!queue.empty()) {
            int i = queue.front();
            queue.pop_front();
            pixels.push_back(i);
            int x = i % w, y = i / w;
            const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto& n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
                int j = n[1] * w + n[0];
                if (!seen[j] && in_region(j)) {
                    seen[j] = 1;
                    queue.push_back(j);
                }
            }
        }
        out.push_back(std::move(pixels));
    }
    return out;
}

// 255 = the part. Background is near-white regions touching the border,
// plus large enclosed near-white regions (a window frame's opening) — real
// part faces are shaded, not pure white. Then specks are dropped and small
// gaps closed.
Gray part_mask(const RgbImage& img) {
    const int w = img.width, h = img.height;
    auto white = [&](int i) {
        const uint8_t* p = &img.pixels[static_cast<size_t>(i) * 3];
        return 255 - std::min({p[0], p[1], p[2]}) <= kBgTolerance;
    };
    Gray part{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h, 255)};
    for (const auto& region : regions(w, h, white)) {
        bool border = std::any_of(region.begin(), region.end(), [&](int i) {
            int x = i % w, y = i / w;
            return x == 0 || y == 0 || x == w - 1 || y == h - 1;
        });
        if (border || region.size() >= kHoleMin * w * h) {
            for (int i : region) part.v[i] = 0;
        }
    }

    part = rank_filter(rank_filter(part, false, 1), true, 1);  // specks
    auto blobs = regions(w, h, [&](int i) { return part.v[i] != 0; });
    size_t largest = 0;
    for (const auto& b : blobs) largest = std::max(largest, b.size());
    for (const auto& b : blobs) {
        if (b.size() < kSpeckMin * largest) {
            for (int i : b) part.v[i] = 0;
        }
    }
    // Close: bridge small gaps where a white edge touches the background.
    return rank_filter(rank_filter(part, true, 3), false, 3);
}

bool is_faint(const RgbImage& img, const Gray& part) {
    std::array<size_t, 256> hist{};
    size_t total = 0;
    for (size_t i = 0; i < part.v.size(); ++i) {
        if (!part.v[i]) continue;
        const uint8_t* p = &img.pixels[i * 3];
        ++hist[(p[0] + p[1] + p[2]) / 3];
        ++total;
    }
    size_t seen = 0;
    for (int level = 0; level < 256; ++level) {
        seen += hist[level];
        if (seen >= total * 0.05) return level >= kFaintPartMin;
    }
    return false;
}

// 255 inside a rounded rectangle filling the image (corner radius w/10).
Gray rounded_tile(int w, int h) {
    Gray t{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h, 0)};
    double r = w / 10.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double cx = std::clamp(x + 0.5, r, w - r), cy = std::clamp(y + 0.5, r, h - r);
            double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            t.at(x, y) = static_cast<uint8_t>(std::clamp((r - d + 0.5) * 255.0, 0.0, 255.0));
        }
    return t;
}

}  // namespace

std::optional<RgbImage> decode_jpeg(const std::string& path) {
    struct FileCloser {
        void operator()(FILE* f) const { std::fclose(f); }
    };
    std::unique_ptr<FILE, FileCloser> file(std::fopen(path.c_str(), "rb"));
    if (!file) return std::nullopt;

    jpeg_decompress_struct cinfo{};
    JpegErrorManager jerr{};
    cinfo.err = jpeg_std_error(&jerr.base);
    jerr.base.error_exit = on_jpeg_error;
    // Nothing between here and the setjmp return needs destructors run
    // besides `image` and `file`, both declared before it.
    RgbImage image;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return std::nullopt;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file.get());
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    image.width = static_cast<int>(cinfo.output_width);
    image.height = static_cast<int>(cinfo.output_height);
    image.pixels.resize(static_cast<size_t>(image.width) * image.height * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = image.pixels.data() +
                       static_cast<size_t>(cinfo.output_scanline) * image.width * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return image;
}

std::optional<RgbImage> backdrop(const RgbImage& img, bool trans, bool light_color) {
    const int w = img.width, h = img.height;
    if (w <= 0 || h <= 0) return std::nullopt;

    std::optional<Gray> part;
    if (!trans) {
        part = part_mask(img);
        if (!light_color && !is_faint(img, *part)) return std::nullopt;
    }

    const int W = w * kScale, H = h * kScale;
    std::vector<uint8_t> photo = upscale(img.pixels, w, h, 3, kScale);
    if (trans) {
        // Deepen the faint edge lines a little. The background is white,
        // so the image's darkest pixel is the part's.
        int darkest = 255;
        for (size_t i = 0; i < img.pixels.size(); i += 3) {
            darkest = std::min(darkest, (img.pixels[i] + img.pixels[i + 1] + img.pixels[i + 2]) / 3);
        }
        double gain = std::min(kTransGain, 150.0 / std::max(1, 255 - darkest));
        for (uint8_t& c : photo) {
            c = static_cast<uint8_t>(std::clamp(255 - (255 - c) * gain, 0.0, 255.0));
        }
    }

    // Smooth, anti-aliased silhouette at the output size, pulled in a pixel
    // so the JPEG's off-white fringe stays in the multiplied band.
    std::optional<Gray> mask;
    if (part) {
        Gray m{W, H, upscale(part->v, w, h, 1, kScale)};
        m = gaussian_blur(m, kScale * 0.8);
        for (uint8_t& v : m.v) v = v >= 128 ? 255 : 0;
        mask = gaussian_blur(rank_filter(m, false, 2), kScale * 0.5);
    }

    const Gray tile = rounded_tile(W, H);
    RgbImage out{W, H, std::vector<uint8_t>(photo.size())};
    for (size_t i = 0; i < tile.v.size(); ++i) {
        double t = tile.v[i] / 255.0;
        double m = mask ? mask->v[i] / 255.0 * t : 0;
        for (int c = 0; c < 3; ++c) {
            double p = photo[i * 3 + c];
            double multiplied = p * kTile[c] / 255.0;
            double v = 255 * (1 - t) + multiplied * t;  // tile over white
            v = v * (1 - m) + p * m;                    // white part on top
            out.pixels[i * 3 + c] = static_cast<uint8_t>(std::lround(v));
        }
    }
    return out;
}

}  // namespace lugbulk::image_backdrop

#include "labels_pdf.h"

#include <curl/curl.h>
#include <podofo/podofo.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

#include "colors.h"
#include "image_backdrop.h"
#include "pdf_text.h"
#include "qrcodegen.hpp"

namespace lugbulk::labels_pdf {

namespace {

constexpr double kMmToPt = 72.0 / 25.4;
constexpr int kImageFetchWorkers = 8;
constexpr long kMissRetrySeconds = 24 * 60 * 60;

// A part photo is ~5 KB; refuse anything absurd.
constexpr size_t kMaxImageBytes = 2 * 1024 * 1024;

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t n = size * nmemb;
    if (out->size() + n > kMaxImageBytes) return 0;  // curl aborts the download
    out->insert(out->end(), ptr, ptr + n);
    return n;
}

bool file_exists_nonempty(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

bool file_exists_empty(const std::string& path, long* mtime_out) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    if (mtime_out) *mtime_out = static_cast<long>(st.st_mtime);
    return st.st_size == 0;
}

// Writes `data` to `path` atomically (temp file + rename), so a concurrent
// request for the same part — or a crash mid-write — never sees a
// half-written image.
bool write_atomically(const std::string& path, const std::vector<uint8_t>& data) {
    static std::atomic<unsigned> counter{0};
    std::string tmp = path + "." + std::to_string(::getpid()) + "." +
                      std::to_string(counter.fetch_add(1)) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out.good()) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Downloads and caches a part thumbnail by element ID. Returns the local
// cache path on success, or empty string on failure (missing product
// photo, network issue, etc.) so rendering can skip gracefully — mirrors
// render_labels.py's _cached_image_path.
std::string cached_image_path(const std::string& element_id, const std::string& url,
                              const std::string& cache_dir) {
    // pivot_sheet only emits digit-only IDs, but this is the one place the
    // ID becomes a filesystem path — never trust it implicitly.
    if (!is_valid_element_id(element_id)) return "";
    std::string path = cache_dir + "/" + element_id + ".jpg";

    if (file_exists_nonempty(path)) return path;

    long mtime = 0;
    if (file_exists_empty(path, &mtime)) {
        long now = static_cast<long>(std::time(nullptr));
        if (now - mtime < kMissRetrySeconds) {
            return "";  // cached miss, not stale enough to retry yet
        }
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    std::vector<uint8_t> data;
    bool ok = false;
    if (curl) {
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Mozilla/5.0");
        CURLcode rc = curl_easy_perform(curl.get());
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        // Must actually be a JPEG (FF D8 FF) — a CDN error page served with
        // a 200 would otherwise be cached and then fail to embed forever.
        ok = rc == CURLE_OK && status >= 200 && status < 300 && data.size() > 3 &&
             data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    }

    if (ok && write_atomically(path, data)) return path;
    // Cache the miss as an empty file so we don't re-fetch every run.
    std::ofstream(path, std::ios::binary | std::ios::trunc);
    return "";
}

// Warms the image cache for all unique element IDs in parallel, so the
// per-label lookups during rendering are just cache hits.
void prefetch_images(const std::vector<LabelRecord>& records, const std::string& cache_dir) {
    std::map<std::string, std::string> unique;  // element_id -> url
    for (const auto& r : records) unique[r.element_id] = r.image_url;

    std::vector<std::pair<std::string, std::string>> items(unique.begin(), unique.end());
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (size_t i; (i = next.fetch_add(1)) < items.size();) {
            cached_image_path(items[i].first, items[i].second, cache_dir);
        }
    };
    int n_workers = static_cast<int>(
        std::min<size_t>(kImageFetchWorkers, std::max<size_t>(1, items.size())));
    std::vector<std::future<void>> futures;
    for (int i = 0; i < n_workers; ++i) {
        futures.push_back(std::async(std::launch::async, worker));
    }
    for (auto& f : futures) f.wait();
}

// Width of `winansi_text` (already WinAnsi-encoded) if drawn at `size` pt
// with `font`. PdfFontMetrics::StringWidth measures at the font's
// currently-set size (PdfFont::SetFontSize), so this mutates that shared
// state — callers must not assume the font's size is unchanged afterward.
double string_width_at(PoDoFo::PdfFont* font, const std::string& winansi_text, double size) {
    font->SetFontSize(static_cast<float>(size));
    return font->GetFontMetrics()->StringWidth(winansi_text.c_str());
}

// Shrinks font size to fit `text` (UTF-8 in, converted to WinAnsi here)
// within max_width_pt; truncates with an ellipsis as a last resort if even
// min_size doesn't fit. Returns the WinAnsi-encoded text and its size.
std::pair<std::string, double> fit_string(PoDoFo::PdfFont* font, const std::string& utf8_text,
                                          double max_size, double min_size, double max_width_pt) {
    std::string text = pdf_text::to_winansi(utf8_text);

    double size = max_size;
    while (size > min_size && string_width_at(font, text, size) > max_width_pt) size -= 0.5;
    if (string_width_at(font, text, size) <= max_width_pt) return {text, size};

    size = min_size;
    std::string truncated = text;
    while (!truncated.empty() && string_width_at(font, truncated + "...", size) > max_width_pt) {
        truncated.pop_back();
    }
    return {truncated.empty() ? text : truncated + "...", size};
}

// Positions and font sizes (points) scaled to the label's height, so every
// label size gets the same proportions. Parts that are switched off free
// their space: lines stack up, and without a photo the text uses the full
// width. Mirrors render_labels._layout in lugbulk-label.
struct Layout {
    double pad, img, qr, text_x, text_right;
    double id_size, small, name_size;
    double y_id, y_name;
    std::vector<double> lines;  // baselines of the stacked small-text lines
};

Layout compute_layout(double width, double height, const LabelOptions& opts, int n_lines) {
    Layout L{};
    L.pad = std::min(height * 0.07, 9.0);
    double inner = height - 2 * L.pad;
    L.id_size = std::min(inner * 0.20, 26.0);
    L.small = std::min(inner * 0.12, 15.0);
    L.name_size = std::min(inner * 0.22, 30.0);
    bool top_row = opts.show(LabelPart::kElementId) || opts.show(LabelPart::kQty);
    bool name_row = opts.show(LabelPart::kName) || opts.show(LabelPart::kCount);

    L.y_id = height - L.pad - L.id_size * 0.8;
    double first = top_row ? L.y_id - L.id_size * 0.2 - L.small * 1.25
                           : height - L.pad - L.small * 0.85;
    for (int i = 0; i < n_lines; ++i) L.lines.push_back(first - i * L.small * 1.2);
    double last_text = !L.lines.empty() ? L.lines.back() : (top_row ? L.y_id : height - L.pad);

    // On taller labels, lift the name toward the text block rather than
    // leaving it stranded at the bottom edge.
    L.y_name = L.pad + L.name_size * 0.22;
    double lift = 0;
    if (name_row) {
        double slack = (last_text - L.small * 0.3) - (L.y_name + L.name_size * 0.75);
        lift = std::max(0.0, slack) * 0.45;
        L.y_name += lift;
    }
    double art_height = inner - (name_row ? L.name_size * 1.3 + lift : 0);
    L.img = opts.show(LabelPart::kPhoto) ? std::min(art_height, width * 0.32) : 0;
    L.qr = opts.show(LabelPart::kQr) ? std::min(art_height * 0.85, width * 0.17) : 0;
    L.text_x = L.img > 0 ? L.pad + L.img + L.pad * 0.8 : L.pad;
    L.text_right = width - L.pad - (L.qr > 0 ? L.qr + L.pad * 0.8 : 0);
    return L;
}

struct Fonts {
    PoDoFo::PdfFont* regular;
    PoDoFo::PdfFont* bold;
};

void draw_text(PoDoFo::PdfPainter& painter, PoDoFo::PdfFont* font, double size, double x,
               double y, const std::string& winansi_text) {
    painter.SetFont(font);
    font->SetFontSize(static_cast<float>(size));
    painter.DrawText(x, y, PoDoFo::PdfString(winansi_text.c_str()));
}

// Loads (once per element, per PDF) the thumbnail to draw: the product
// photo, or for light parts a copy on a gray tile (see image_backdrop.h).
PoDoFo::PdfImage* label_image(PoDoFo::PdfStreamedDocument& doc, const LabelRecord& record,
                              const std::string& cache_dir, bool backdrop,
                              std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>>& cache) {
    auto it = cache.find(record.element_id);
    if (it != cache.end()) return it->second.get();

    std::unique_ptr<PoDoFo::PdfImage> img;
    std::string path = cached_image_path(record.element_id, record.image_url, cache_dir);
    if (!path.empty()) {
        try {
            std::optional<image_backdrop::RgbImage> tiled;
            if (!backdrop) {
                // Plain product photo requested.
            } else if (auto decoded = image_backdrop::decode_jpeg(path)) {
                tiled = image_backdrop::backdrop(*decoded,
                                                 colors::is_transparent(record.lego_color, record.bl_color),
                                                 colors::is_light(record.lego_color, record.bl_color));
            }
            img = std::make_unique<PoDoFo::PdfImage>(&doc);
            if (tiled) {
                img->SetImageColorSpace(PoDoFo::ePdfColorSpace_DeviceRGB);
                PoDoFo::PdfMemoryInputStream stream(reinterpret_cast<const char*>(tiled->pixels.data()),
                                                    static_cast<PoDoFo::pdf_long>(tiled->pixels.size()));
                img->SetImageData(static_cast<unsigned>(tiled->width),
                                  static_cast<unsigned>(tiled->height), 8, &stream);
            } else {
                img->LoadFromJpeg(path.c_str());
            }
        } catch (const PoDoFo::PdfError&) {
            img.reset();  // corrupt/unreadable image; skip thumbnail, keep text
        }
    }
    PoDoFo::PdfImage* raw = img.get();
    cache.emplace(record.element_id, std::move(img));
    return raw;
}

// Draws a QR code of `data` as filled modules, bottom-left at (x, y).
void draw_qr(PoDoFo::PdfPainter& painter, double x, double y, double size, const std::string& data) {
    auto qr = qrcodegen::QrCode::encodeText(data.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    int n = qr.getSize();
    double m = size / n;
    painter.SetColor(0, 0, 0);
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < n; ++col) {
            if (qr.getModule(col, row)) {
                // Slight overlap so adjacent modules don't show hairline gaps.
                painter.Rectangle(x + col * m, y + (n - 1 - row) * m, m * 1.02, m * 1.02);
            }
        }
    }
    painter.Fill();
}

void draw_label(PoDoFo::PdfPainter& painter, PoDoFo::PdfStreamedDocument& doc, double origin_x,
                double origin_y, double width, double height, const LabelRecord& record,
                const LabelOptions& opts, const std::string& cache_dir, const Fonts& fonts,
                std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>>& image_cache) {
    // PoDoFo draws in absolute page coordinates (origin bottom-left), so
    // every position below is origin + label-local offset.
    auto X = [&](double local_x) { return origin_x + local_x; };
    auto Y = [&](double local_y) { return origin_y + local_y; };

    std::vector<std::string> color_lines;
    if (opts.show(LabelPart::kLegoColor) && !record.lego_color.empty())
        color_lines.push_back("LEGO: " + record.lego_color);
    if (opts.show(LabelPart::kBlColor) && !record.bl_color.empty())
        color_lines.push_back("BL: " + record.bl_color);
    std::vector<std::string> texts = color_lines;
    if (opts.show(LabelPart::kDescription) && !record.description.empty())
        texts.push_back(record.description);
    const Layout L = compute_layout(width, height, opts, static_cast<int>(texts.size()));

    if (L.img > 0) {
        if (PoDoFo::PdfImage* img = label_image(doc, record, cache_dir,
                                                opts.show(LabelPart::kBackdrop), image_cache)) {
            painter.DrawImage(X(L.pad), Y(height - L.pad - L.img), img, L.img / img->GetWidth(),
                              L.img / img->GetHeight());
        }
    }
    if (L.qr > 0) {
        draw_qr(painter, X(width - L.pad - L.qr), Y(height - L.pad - L.qr), L.qr,
                bricklink_url(record.element_id));
    }

    const double text_x = L.text_x, right = L.text_right, text_max = right - text_x;

    // Element ID and qty share the top row. Shrink both together until
    // they fit — the ID must never be cut short.
    std::string id_text = opts.show(LabelPart::kElementId) ? pdf_text::to_winansi(record.element_id) : "";
    std::string qty_text = opts.show(LabelPart::kQty) ? pdf_text::to_winansi("Qty: " + record.qty) : "";
    auto row_width = [&](double k) {
        return (id_text.empty() ? 0 : string_width_at(fonts.bold, id_text, L.id_size * k)) +
               (qty_text.empty() ? 0 : string_width_at(fonts.bold, qty_text, L.id_size * 0.85 * k)) +
               (!id_text.empty() && !qty_text.empty() ? L.pad : 0);
    };
    double scale = 1.0;
    while (scale > 0.4 && row_width(scale) > text_max) scale -= 0.05;
    if (!qty_text.empty()) {
        double size = L.id_size * 0.85 * scale;
        double w = string_width_at(fonts.bold, qty_text, size);
        draw_text(painter, fonts.bold, size, X(right - w), Y(L.y_id), qty_text);
    }
    if (!id_text.empty()) draw_text(painter, fonts.bold, L.id_size * scale, X(text_x), Y(L.y_id), id_text);

    // Swatch: a square of the part's color beside the color name lines.
    double swatch_w = 0;
    auto rgb = opts.show(LabelPart::kSwatch) ? colors::swatch_rgb(record.lego_color, record.bl_color)
                                             : std::nullopt;
    if (rgb && !color_lines.empty()) {
        double side = L.small * 1.2 * (color_lines.size() - 1) + L.small * 0.95;
        double bottom = L.lines[color_lines.size() - 1] - L.small * 0.22;
        painter.SetColor((*rgb)[0], (*rgb)[1], (*rgb)[2]);
        painter.Rectangle(X(text_x), Y(bottom), side, side);
        painter.Fill();
        painter.SetStrokingColor(0.35, 0.35, 0.35);
        painter.SetStrokeWidth(0.5);
        painter.Rectangle(X(text_x), Y(bottom), side, side);
        painter.Stroke();
        if (colors::is_transparent(record.lego_color, record.bl_color)) {
            // Mark see-through colors with a diagonal, like a pane of glass.
            painter.DrawLine(X(text_x), Y(bottom), X(text_x + side), Y(bottom + side));
        }
        painter.SetColor(0, 0, 0);
        swatch_w = side + L.pad * 0.5;
    }

    for (size_t i = 0; i < texts.size(); ++i) {
        double x = text_x + (i < color_lines.size() ? swatch_w : 0);
        auto [fitted, size] = fit_string(fonts.regular, texts[i], L.small, L.small * 0.7, right - x);
        draw_text(painter, fonts.regular, size, X(x), Y(L.lines[i]), fitted);
    }

    double counter_w = 0;
    if (opts.show(LabelPart::kCount) && record.part_total > 0) {
        std::string counter = std::to_string(record.part_seq) + " of " +
                              std::to_string(record.part_total);
        counter_w = string_width_at(fonts.bold, counter, L.small);
        draw_text(painter, fonts.bold, L.small, X(width - L.pad - counter_w), Y(L.y_name), counter);
    }

    if (opts.show(LabelPart::kName)) {
        // Centered on the label; kept clear of the counter on both sides so
        // it stays visually centered.
        double name_max = width - 2 * L.pad - 2 * (counter_w + L.pad);
        auto [name_text, name_size] =
            fit_string(fonts.bold, record.person, L.name_size, L.name_size * 0.55, name_max);
        double name_w = string_width_at(fonts.bold, name_text, name_size);
        draw_text(painter, fonts.bold, name_size, X((width - name_w) / 2), Y(L.y_name), name_text);
    }
}

// Where every label on a page goes, in page coordinates: calls
// place(x, y, w, h, slot) for slots [0, count).
template <typename F>
void for_each_slot(const layout::LabelSpec& spec, int count, F place) {
    const double page_h = spec.sheet_height_mm * kMmToPt;
    const double label_w = spec.label_width_mm * kMmToPt;
    const double label_h = spec.label_height_mm * kMmToPt;
    for (int slot = 0; slot < count; ++slot) {
        int col = slot % spec.columns;
        int row = slot / spec.columns;
        double x = spec.left_margin_mm * kMmToPt + col * (label_w + spec.column_gap_mm * kMmToPt);
        // PDF y-origin is bottom-left; sheet layout is defined top-down.
        double y_top = page_h - spec.top_margin_mm * kMmToPt -
                       row * (label_h + spec.row_gap_mm * kMmToPt);
        place(x, y_top - label_h, label_w, label_h, slot);
    }
}

}  // namespace

namespace {

std::vector<uint8_t> finish(PoDoFo::PdfStreamedDocument& doc, PoDoFo::PdfRefCountedBuffer& buffer) {
    doc.Close();
    std::vector<uint8_t> out(buffer.GetSize());
    std::memcpy(out.data(), buffer.GetBuffer(), buffer.GetSize());
    return out;
}

}  // namespace

std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                      const std::string& image_cache_dir,
                                      const layout::LabelSpec& spec, const LabelOptions& opts,
                                      size_t max_pages) {
    ::mkdir(image_cache_dir.c_str(), 0755);  // ignore EEXIST
    const int per_sheet = spec.per_sheet();
    size_t count = records.size();
    if (max_pages > 0) count = std::min(count, max_pages * per_sheet);
    std::vector<LabelRecord> used(records.begin(), records.begin() + static_cast<long>(count));
    if (opts.show(LabelPart::kPhoto)) prefetch_images(used, image_cache_dir);

    PoDoFo::PdfRefCountedBuffer buffer;
    PoDoFo::PdfOutputDevice device(&buffer);
    PoDoFo::PdfStreamedDocument doc(&device);
    Fonts fonts{doc.CreateFont("Helvetica"), doc.CreateFont("Helvetica-Bold")};
    if (!fonts.regular || !fonts.bold) {
        throw std::runtime_error("pdf error: could not load base fonts");
    }
    std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>> image_cache;

    const double page_w = spec.sheet_width_mm * kMmToPt, page_h = spec.sheet_height_mm * kMmToPt;
    size_t idx = 0;
    size_t total_pages = used.empty() ? 1 : (used.size() + per_sheet - 1) / per_sheet;
    for (size_t page = 0; page < total_pages; ++page) {
        PoDoFo::PdfPage* pdf_page = doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
        PoDoFo::PdfPainter painter;
        painter.SetPage(pdf_page);
        int on_page = static_cast<int>(std::min<size_t>(per_sheet, used.size() - idx));
        for_each_slot(spec, on_page, [&](double x, double y, double w, double h, int) {
            draw_label(painter, doc, x, y, w, h, used[idx++], opts, image_cache_dir, fonts,
                       image_cache);
        });
        painter.FinishPage();
    }
    return finish(doc, buffer);
}

std::vector<uint8_t> build_test_page(const layout::LabelSpec& spec) {
    PoDoFo::PdfRefCountedBuffer buffer;
    PoDoFo::PdfOutputDevice device(&buffer);
    PoDoFo::PdfStreamedDocument doc(&device);
    PoDoFo::PdfFont* font = doc.CreateFont("Helvetica");
    if (!font) throw std::runtime_error("pdf error: could not load base fonts");

    PoDoFo::PdfPage* pdf_page = doc.CreatePage(
        PoDoFo::PdfRect(0, 0, spec.sheet_width_mm * kMmToPt, spec.sheet_height_mm * kMmToPt));
    PoDoFo::PdfPainter painter;
    painter.SetPage(pdf_page);
    painter.SetStrokingColor(0, 0, 0);
    char size[48];
    std::snprintf(size, sizeof(size), "%.2f\" x %.2f\"", spec.label_height_mm / 25.4,
                  spec.label_width_mm / 25.4);
    for_each_slot(spec, spec.per_sheet(), [&](double x, double y, double w, double h, int slot) {
        // Label outline, and a crosshair at its centre.
        painter.SetStrokeWidth(0.75);
        painter.Rectangle(x + 0.5, y + 0.5, w - 1, h - 1, 4, 4);
        painter.Stroke();
        painter.SetStrokeWidth(0.5);
        painter.DrawLine(x + w / 2 - 6, y + h / 2, x + w / 2 + 6, y + h / 2);
        painter.DrawLine(x + w / 2, y + h / 2 - 6, x + w / 2, y + h / 2 + 6);
        double text_size = std::min(h * 0.12, 10.0);
        std::string text = pdf_text::to_winansi(spec.brand + " " + spec.part + " #" +
                                                std::to_string(slot + 1) + "  " + size);
        double tw = string_width_at(font, text, text_size);
        draw_text(painter, font, text_size, x + (w - tw) / 2, y + h / 2 - text_size * 2.2, text);
    });
    painter.FinishPage();
    return finish(doc, buffer);
}

std::optional<LabelOptions> LabelOptions::parse(const std::string& hide, const std::string& show,
                                                std::string* error) {
    LabelOptions opts;
    for (const auto& [list, hidden] : {std::pair{&hide, true}, std::pair{&show, false}}) {
        size_t start = 0;
        while (start <= list->size()) {
            size_t end = list->find(',', start);
            if (end == std::string::npos) end = list->size();
            std::string name = list->substr(start, end - start);
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            if (!name.empty()) {
                auto part = label_part_from_name(name);
                if (!part) {
                    if (error) *error = "unknown label part '" + name + "'";
                    return std::nullopt;
                }
                opts.set(*part, !hidden);
            }
            start = end + 1;
        }
    }
    return opts;
}

std::optional<LabelOptions> LabelOptions::from_hidden(const std::string& hidden,
                                                      std::string* error) {
    // Show everything (including the QR code), then hide what's listed.
    auto opts = parse(hidden, "", error);
    if (!opts) return std::nullopt;
    bool qr_listed = false;
    size_t start = 0;
    while (start <= hidden.size()) {
        size_t end = hidden.find(',', start);
        if (end == std::string::npos) end = hidden.size();
        std::string name = hidden.substr(start, end - start);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        qr_listed |= name == "qr";
        start = end + 1;
    }
    opts->set(LabelPart::kQr, !qr_listed);
    return opts;
}

std::string LabelOptions::hidden_csv() const {
    std::string out;
    for (size_t i = 0; i < kLabelPartNames.size(); ++i) {
        if (!show(static_cast<LabelPart>(i))) out += (out.empty() ? "" : ",") + std::string(kLabelPartNames[i]);
    }
    return out;
}

std::optional<LabelPart> label_part_from_name(std::string_view name) {
    for (size_t i = 0; i < kLabelPartNames.size(); ++i) {
        if (kLabelPartNames[i] == name) return static_cast<LabelPart>(i);
    }
    return std::nullopt;
}

std::string bricklink_url(const std::string& element_id) {
    return "https://www.bricklink.com/v2/search.page?q=" + element_id;
}

}  // namespace lugbulk::labels_pdf

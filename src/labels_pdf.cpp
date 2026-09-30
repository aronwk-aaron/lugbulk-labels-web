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

namespace lugbulk::labels_pdf {

namespace {

constexpr double kMmToPt = 72.0 / 25.4;
constexpr int kImageFetchWorkers = 8;
constexpr long kMissRetrySeconds = 24 * 60 * 60;

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t n = size * nmemb;
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
// label size gets the same proportions. Mirrors render_labels._layout.
struct Layout {
    double pad, img, text_x;
    double id_size, small, name_size;
    double y_id, y_lego, y_bl, y_desc, y_name;
};

Layout compute_layout(double width, double height) {
    Layout L{};
    L.pad = std::min(height * 0.07, 9.0);
    double inner = height - 2 * L.pad;
    L.id_size = std::min(inner * 0.20, 26.0);
    L.small = std::min(inner * 0.12, 15.0);
    L.name_size = std::min(inner * 0.22, 30.0);

    L.y_id = height - L.pad - L.id_size * 0.8;
    L.y_lego = L.y_id - L.id_size * 0.2 - L.small * 1.25;
    L.y_bl = L.y_lego - L.small * 1.2;
    L.y_desc = L.y_bl - L.small * 1.2;
    // On taller labels, lift the name toward the text block rather than
    // leaving it stranded at the bottom edge.
    L.y_name = L.pad + L.name_size * 0.22;
    double slack = (L.y_desc - L.small * 0.3) - (L.y_name + L.name_size * 0.75);
    double lift = std::max(0.0, slack) * 0.45;
    L.y_name += lift;
    L.img = std::min(inner - L.name_size * 1.3 - lift, width * 0.32);
    L.text_x = L.pad + L.img + L.pad * 0.8;
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
                              const std::string& cache_dir,
                              std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>>& cache) {
    auto it = cache.find(record.element_id);
    if (it != cache.end()) return it->second.get();

    std::unique_ptr<PoDoFo::PdfImage> img;
    std::string path = cached_image_path(record.element_id, record.image_url, cache_dir);
    if (!path.empty()) {
        try {
            std::optional<image_backdrop::RgbImage> tiled;
            if (auto decoded = image_backdrop::decode_jpeg(path)) {
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

void draw_label(PoDoFo::PdfPainter& painter, PoDoFo::PdfStreamedDocument& doc, double origin_x,
                double origin_y, double width, double height, const LabelRecord& record,
                const std::string& cache_dir, const Fonts& fonts,
                std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>>& image_cache) {
    // PoDoFo draws in absolute page coordinates (origin bottom-left), so
    // every position below is origin + label-local offset.
    auto X = [&](double local_x) { return origin_x + local_x; };
    auto Y = [&](double local_y) { return origin_y + local_y; };
    const Layout L = compute_layout(width, height);

    if (PoDoFo::PdfImage* img = label_image(doc, record, cache_dir, image_cache)) {
        painter.DrawImage(X(L.pad), Y(height - L.pad - L.img), img, L.img / img->GetWidth(),
                          L.img / img->GetHeight());
    }

    const double text_max = width - L.pad - L.text_x;

    std::string qty_text = pdf_text::to_winansi("Qty: " + record.qty);
    double qty_size = L.id_size * 0.85;
    double qty_w = string_width_at(fonts.bold, qty_text, qty_size);
    draw_text(painter, fonts.bold, qty_size, X(width - L.pad - qty_w), Y(L.y_id), qty_text);

    auto [id_text, id_size] = fit_string(fonts.bold, record.element_id, L.id_size,
                                         L.id_size * 0.6, text_max - qty_w - L.pad);
    draw_text(painter, fonts.bold, id_size, X(L.text_x), Y(L.y_id), id_text);

    const std::pair<double, std::string> lines[] = {
        {L.y_lego, record.lego_color.empty() ? "" : "LEGO: " + record.lego_color},
        {L.y_bl, record.bl_color.empty() ? "" : "BL: " + record.bl_color},
        {L.y_desc, record.description},
    };
    for (const auto& [y, text] : lines) {
        if (text.empty()) continue;
        auto [fitted, size] = fit_string(fonts.regular, text, L.small, L.small * 0.7, text_max);
        draw_text(painter, fonts.regular, size, X(L.text_x), Y(y), fitted);
    }

    double counter_w = 0;
    if (record.part_total > 0) {
        std::string counter = std::to_string(record.part_seq) + " of " +
                              std::to_string(record.part_total);
        counter_w = string_width_at(fonts.bold, counter, L.small);
        draw_text(painter, fonts.bold, L.small, X(width - L.pad - counter_w), Y(L.y_name), counter);
    }

    // Centered on the label; kept clear of the counter on both sides so it
    // stays visually centered.
    double name_max = width - 2 * L.pad - 2 * (counter_w + L.pad);
    auto [name_text, name_size] =
        fit_string(fonts.bold, record.person, L.name_size, L.name_size * 0.55, name_max);
    double name_w = string_width_at(fonts.bold, name_text, name_size);
    draw_text(painter, fonts.bold, name_size, X((width - name_w) / 2), Y(L.y_name), name_text);
}

}  // namespace

std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                      const std::string& image_cache_dir,
                                      const layout::LabelSpec& spec) {
    ::mkdir(image_cache_dir.c_str(), 0755);  // ignore EEXIST
    prefetch_images(records, image_cache_dir);

    const double page_w = spec.sheet_width_mm * kMmToPt;
    const double page_h = spec.sheet_height_mm * kMmToPt;
    const double label_w = spec.label_width_mm * kMmToPt;
    const double label_h = spec.label_height_mm * kMmToPt;
    const double left_margin = spec.left_margin_mm * kMmToPt;
    const double top_margin = spec.top_margin_mm * kMmToPt;
    const double col_gap = spec.column_gap_mm * kMmToPt;
    const double row_gap = spec.row_gap_mm * kMmToPt;
    const int per_sheet = spec.per_sheet();

    PoDoFo::PdfRefCountedBuffer buffer;
    PoDoFo::PdfOutputDevice device(&buffer);
    PoDoFo::PdfStreamedDocument doc(&device);

    Fonts fonts{doc.CreateFont("Helvetica"), doc.CreateFont("Helvetica-Bold")};
    if (!fonts.regular || !fonts.bold) {
        throw std::runtime_error("pdf error: could not load base fonts");
    }

    std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>> image_cache;

    size_t idx = 0;
    int total_pages = records.empty() ? 1 : static_cast<int>(
                                                (records.size() + per_sheet - 1) / per_sheet);

    for (int page = 0; page < total_pages; ++page) {
        PoDoFo::PdfPage* pdf_page = doc.CreatePage(PoDoFo::PdfRect(0, 0, page_w, page_h));
        PoDoFo::PdfPainter painter;
        painter.SetPage(pdf_page);

        for (int slot = 0; slot < per_sheet && idx < records.size(); ++slot, ++idx) {
            int col = slot % spec.columns;
            int row = slot / spec.columns;
            double x = left_margin + col * (label_w + col_gap);
            // PDF y-origin is bottom-left; sheet layout is defined top-down.
            double y_top = page_h - top_margin - row * (label_h + row_gap);
            double y = y_top - label_h;

            draw_label(painter, doc, x, y, label_w, label_h, records[idx], image_cache_dir, fonts,
                       image_cache);
        }

        painter.FinishPage();
    }

    doc.Close();

    std::vector<uint8_t> out(buffer.GetSize());
    std::memcpy(out.data(), buffer.GetBuffer(), buffer.GetSize());
    return out;
}

}  // namespace lugbulk::labels_pdf

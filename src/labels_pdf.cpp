#include "labels_pdf.h"

#include <curl/curl.h>
#include <podofo/podofo.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <sstream>
#include <sys/stat.h>

#include "pdf_text.h"
#include "sheet_layout.h"

namespace lugbulk::labels_pdf {

namespace {

constexpr double kMmToPt = 72.0 / 25.4;
constexpr double kImgSizePt = 16.0 * kMmToPt;
constexpr double kTextLeftPt = kImgSizePt + 3.0 * kMmToPt;
constexpr double kMarginPt = 2.0;
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

// Downloads and caches a part thumbnail by element ID. Returns the local
// cache path on success, or empty string on failure (missing product
// photo, network issue, etc.) so rendering can skip gracefully — mirrors
// render_labels.py's _cached_image_path.
std::string cached_image_path(const std::string& element_id, const std::string& url,
                               const std::string& cache_dir) {
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
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Mozilla/5.0");
        struct curl_slist* headers = nullptr;
        CURLcode rc = curl_easy_perform(curl.get());
        curl_slist_free_all(headers);
        long status = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
        ok = (rc == CURLE_OK && status >= 200 && status < 300 && !data.empty());
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (ok) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return out.good() ? path : "";
    }
    // Cache the miss as an empty file so we don't re-fetch every run.
    out.close();
    return "";
}

// Warms the image cache for all unique element IDs in parallel, so the
// per-label lookups during rendering are just cache hits.
void prefetch_images(const std::vector<LabelRecord>& records, const std::string& cache_dir) {
    std::map<std::string, std::string> unique;  // element_id -> url
    for (const auto& r : records) unique[r.element_id] = r.image_url;

    std::vector<std::future<void>> futures;
    std::vector<std::pair<std::string, std::string>> items(unique.begin(), unique.end());
    size_t next = 0;
    std::mutex m;
    auto worker = [&]() {
        for (;;) {
            std::pair<std::string, std::string> item;
            {
                std::lock_guard<std::mutex> lock(m);
                if (next >= items.size()) return;
                item = items[next++];
            }
            cached_image_path(item.first, item.second, cache_dir);
        }
    };
    int n_workers = std::min<int>(kImageFetchWorkers, std::max<size_t>(1, items.size()));
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
// min_size doesn't fit. Returns the WinAnsi-encoded text ready for
// PdfString/DrawText, and the font size to draw it at — leaves `font`'s
// size set to the returned size as a side effect (matches how the caller
// immediately uses it for DrawText).
std::pair<std::string, double> fit_string(PoDoFo::PdfFont* font, const std::string& utf8_text,
                                           double max_size, double min_size, double max_width_pt) {
    std::string text = pdf_text::to_winansi(utf8_text);

    double size = max_size;
    while (size > min_size && string_width_at(font, text, size) > max_width_pt) size -= 0.5;
    if (string_width_at(font, text, size) <= max_width_pt) return {text, size};

    size = min_size;
    std::string truncated = text;
    while (!truncated.empty() &&
           string_width_at(font, truncated + "...", size) > max_width_pt) {
        truncated.pop_back();
    }
    return {truncated.empty() ? text : truncated + "...", size};
}

void draw_label(PoDoFo::PdfPainter& painter, PoDoFo::PdfStreamedDocument& doc, double origin_x,
                double origin_y, double width_pt, double height_pt, const LabelRecord& record,
                const std::string& cache_dir, PoDoFo::PdfFont* font_regular,
                PoDoFo::PdfFont* font_bold,
                std::map<std::string, std::unique_ptr<PoDoFo::PdfImage>>& image_cache) {
    // Label-local coordinate helper: PoDoFo draws in absolute page
    // coordinates, so every position below is origin + local offset.
    auto X = [&](double local_x) { return origin_x + local_x; };
    auto Y = [&](double local_y) { return origin_y + local_y; };

    std::string img_path = cached_image_path(record.element_id, record.image_url, cache_dir);
    if (!img_path.empty()) {
        try {
            PoDoFo::PdfImage* img;
            auto it = image_cache.find(record.element_id);
            if (it != image_cache.end()) {
                img = it->second.get();
            } else {
                auto owned = std::make_unique<PoDoFo::PdfImage>(&doc);
                owned->LoadFromJpeg(img_path.c_str());
                img = owned.get();
                image_cache.emplace(record.element_id, std::move(owned));
            }
            double scale_x = kImgSizePt / img->GetWidth();
            double scale_y = kImgSizePt / img->GetHeight();
            painter.DrawImage(X(kMarginPt), Y(height_pt - kImgSizePt - kMarginPt), img, scale_x,
                               scale_y);
        } catch (const PoDoFo::PdfError&) {
            // Corrupt/unreadable image; skip thumbnail, keep text.
        }
    }

    double text_x = kTextLeftPt;
    double text_max_width = width_pt - text_x - kMarginPt;

    // Element ID (left) and Qty (right) share a text row — Qty is drawn
    // first so its measured width can push the ID's max width in if the
    // element id happens to be long, keeping the two from overlapping.
    std::string id_text = pdf_text::to_winansi("Element ID: " + record.element_id);
    std::string qty_text = pdf_text::to_winansi("Qty: " + record.qty);

    double qty_size = 11.0;
    double qty_width = string_width_at(font_bold, qty_text, qty_size);
    double qty_x = width_pt - kMarginPt - qty_width;

    double id_size = 7.0;
    double id_max_width = std::max(0.0, qty_x - text_x - kMarginPt);
    while (id_size > 5.0 && string_width_at(font_regular, id_text, id_size) > id_max_width) {
        id_size -= 0.5;
    }

    painter.SetFont(font_regular);
    font_regular->SetFontSize(static_cast<float>(id_size));
    painter.DrawText(X(text_x), Y(height_pt - 10), PoDoFo::PdfString(id_text.c_str()));

    painter.SetFont(font_bold);
    font_bold->SetFontSize(static_cast<float>(qty_size));
    painter.DrawText(X(std::max(text_x, qty_x)), Y(height_pt - 10), PoDoFo::PdfString(qty_text.c_str()));

    auto [color_text, color_size] = fit_string(font_regular, record.color, 7, 5, text_max_width);
    painter.SetFont(font_regular);
    font_regular->SetFontSize(static_cast<float>(color_size));
    painter.DrawText(X(text_x), Y(height_pt - 18), PoDoFo::PdfString(color_text.c_str()));

    auto [desc_text, desc_size] =
        fit_string(font_regular, record.description, 7, 5, text_max_width);
    painter.SetFont(font_regular);
    font_regular->SetFontSize(static_cast<float>(desc_size));
    painter.DrawText(X(text_x), Y(height_pt - 26), PoDoFo::PdfString(desc_text.c_str()));

    auto [name_text, name_size] =
        fit_string(font_bold, record.person, 13, 8, width_pt - 2 * kMarginPt);
    painter.SetFont(font_bold);
    font_bold->SetFontSize(static_cast<float>(name_size));
    double name_width = string_width_at(font_bold, name_text, name_size);
    double name_x = (width_pt - name_width) / 2;
    painter.DrawText(X(name_x), Y(height_pt - 46), PoDoFo::PdfString(name_text.c_str()));
}

}  // namespace

std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                       const std::string& image_cache_dir) {
    ::mkdir(image_cache_dir.c_str(), 0755);  // ignore EEXIST
    prefetch_images(records, image_cache_dir);

    const layout::LabelSpec& spec = layout::kAvery5160;
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

    PoDoFo::PdfFont* font_regular = doc.CreateFont("Helvetica");
    PoDoFo::PdfFont* font_bold = doc.CreateFont("Helvetica-Bold");
    if (!font_regular || !font_bold) {
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

            draw_label(painter, doc, x, y, label_w, label_h, records[idx], image_cache_dir,
                       font_regular, font_bold, image_cache);
        }

        painter.FinishPage();
    }

    doc.Close();

    std::vector<uint8_t> out(buffer.GetSize());
    std::memcpy(out.data(), buffer.GetBuffer(), buffer.GetSize());
    return out;
}

}  // namespace lugbulk::labels_pdf

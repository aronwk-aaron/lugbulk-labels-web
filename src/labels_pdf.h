// Renders LabelRecords onto label-sheet (or label-roll) PDFs — port of
// lugbulk-label's render_labels.py. Layout, scaled to the label size:
//
//     [thumb]  6225242 (bold)            Qty: 150
//              LEGO: Medium Stone Grey
//              BL: Light Bluish Gray
//              BRICK 1X1X1 2/3 W/2 KNOBS
//          Person Name (bold, centered)    3 of 10
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

#include "sheet_layout.h"
#include "sheet_pivot.h"

namespace lugbulk::labels_pdf {

// Parts of a label that can be switched on and off (the dashboard's design
// switches; the CLI's --hide/--show). Names match the CLI's LABEL_PARTS.
enum class LabelPart {
    kPhoto, kElementId, kQty, kLegoColor, kBlColor, kDescription, kName, kCount, kBackdrop,
    kSwatch, kQr,
};
inline constexpr std::array<std::string_view, 11> kLabelPartNames{
    "photo", "element_id", "qty", "lego_color", "bl_color", "description", "name", "count",
    "backdrop", "swatch", "qr"};

std::optional<LabelPart> label_part_from_name(std::string_view name);

// Which parts to draw. Everything is on by default except the QR code.
class LabelOptions {
public:
    LabelOptions() { set(LabelPart::kQr, false); }
    bool show(LabelPart p) const { return !hidden_[static_cast<size_t>(p)]; }
    void set(LabelPart p, bool shown) { hidden_[static_cast<size_t>(p)] = !shown; }

    // Defaults, then comma-separated part names to hide and to show. On an
    // unknown name returns nullopt and sets *error.
    static std::optional<LabelOptions> parse(const std::string& hide, const std::string& show = "",
                                             std::string* error = nullptr);
    // Everything on except the comma-separated parts listed.
    static std::optional<LabelOptions> from_hidden(const std::string& hidden,
                                                   std::string* error = nullptr);
    // The hidden parts, comma-separated (what from_hidden round-trips).
    std::string hidden_csv() const;

private:
    std::array<bool, kLabelPartNames.size()> hidden_{};
};

// Where a label's QR code points: BrickLink's search, which resolves LEGO
// element IDs to the right part and color.
std::string bricklink_url(const std::string& element_id);

// The element id in a photo file name, "<element id>.jpg" (the /img/ route),
// or nullopt unless the id passes is_valid_element_id — so the name can
// only ever map to a cache file and a fixed-host LEGO CDN URL.
std::optional<std::string> element_id_from_image_name(const std::string& name);

// What the image cache already knows about a part photo, without fetching:
// kHit (the photo is at *path_out), kMiss (a recent cached miss: LEGO has
// no photo, don't ask again yet) or kUnknown (needs a download). An
// invalid element id is a kMiss with an empty path.
enum class CachedImage { kHit, kMiss, kUnknown };
CachedImage probe_image_cache(const std::string& element_id, const std::string& cache_dir,
                              std::string* path_out = nullptr);

// The cached photo's path, downloading it from `url` first if needed
// (layout::image_url_for). Empty on failure (no photo, network trouble);
// a failure is cached as described below.
std::string cached_image_path(const std::string& element_id, const std::string& url,
                              const std::string& cache_dir);

// `image_cache_dir` is where part thumbnails are cached across runs/sheets
// (keyed by element id, shared across all sheets — LEGO element photos
// aren't sheet- or user-specific). A cached miss (404/timeout/etc.) is
// stored as an empty file and retried after 24h, same as the CLI, so a
// transient CDN outage doesn't permanently blank out a thumbnail.
//
// Records are drawn in the order given (see ordering::order_records).
// `max_pages` > 0 stops after that many pages (the live preview).
// Returns the built PDF as an in-memory buffer — nothing is written to
// disk except the (non-sensitive, shared) image cache.
std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                      const std::string& image_cache_dir,
                                      const layout::LabelSpec& spec,
                                      const LabelOptions& opts = LabelOptions(),
                                      size_t max_pages = 0);

// One page with every label position outlined and numbered, to print on
// plain paper and hold against the label stock to check alignment.
std::vector<uint8_t> build_test_page(const layout::LabelSpec& spec);

// Where everything on one label goes, in label-local points (origin at the
// label's bottom-left) — what draw_label draws, decided without PoDoFo so
// tests/golden.cpp can dump it for the browser port's parity test
// (static/js/labels.js layoutLabel gives the same numbers).
struct LaidText {
    bool bold = false;
    double size = 0, x = 0, y = 0;  // y is the baseline
    std::string text;               // WinAnsi
};
struct LabelDrawing {
    struct Square { double x = 0, y = 0, size = 0; };
    std::optional<Square> image;  // the photo's box
    std::optional<Square> qr;     // the QR code's box (of bricklink_url)
    struct Swatch { double x = 0, y = 0, side = 0; std::array<double, 3> rgb{}; bool trans = false; };
    std::optional<Swatch> swatch;
    std::vector<LaidText> texts;  // in drawing order
};
// A line's width in points: (bold?, WinAnsi text, font size).
using MeasureFn = std::function<double(bool, const std::string&, double)>;
LabelDrawing layout_label(const LabelRecord& record, double width, double height,
                          const LabelOptions& opts, const MeasureFn& measure);
// PoDoFo's Helvetica / Helvetica-Bold metrics, the ones the PDFs use.
MeasureFn helvetica_measure();

// How one label field's text is drawn: its lines (WinAnsi), font size and
// the distance between baselines.
struct FittedText {
    std::vector<std::string> lines;
    double size = 0;
    double leading = 0;
};

// Fits `winansi_text` into a field `max_width` wide: one line, shrunk from
// `max_size` down to `min_size` in half-point steps, when that fits (the
// usual case, drawn exactly as before). Otherwise it wraps onto more lines
// and shrinks further until the lines fit `max_height` (cap height of the
// first line to the descenders of the last). Nothing is ever cut off.
// `width(text, size)` measures a line.
FittedText fit_text(const std::string& winansi_text, double max_size, double min_size,
                    double max_width, double max_height,
                    const std::function<double(const std::string&, double)>& width);

}  // namespace lugbulk::labels_pdf

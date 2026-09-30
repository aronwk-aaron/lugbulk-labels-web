// Renders LabelRecords onto label-sheet (or label-roll) PDFs — port of
// lugbulk-label's render_labels.py. Layout, scaled to the label size:
//
//     [thumb]  6225242 (bold)            Qty: 150
//              LEGO: Medium Stone Grey
//              BL: Light Bluish Gray
//              BRICK 1X1X1 2/3 W/2 KNOBS
//          Person Name (bold, centered)    3 of 10
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sheet_layout.h"
#include "sheet_pivot.h"

namespace lugbulk::labels_pdf {

// `image_cache_dir` is where part thumbnails are cached across runs/sheets
// (keyed by element id, shared across all sheets — LEGO element photos
// aren't sheet- or user-specific). A cached miss (404/timeout/etc.) is
// stored as an empty file and retried after 24h, same as the CLI, so a
// transient CDN outage doesn't permanently blank out a thumbnail.
//
// Records are drawn in the order given (see ordering::order_records).
// Returns the built PDF as an in-memory buffer — nothing is written to
// disk except the (non-sensitive, shared) image cache.
std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                      const std::string& image_cache_dir,
                                      const layout::LabelSpec& spec = layout::kDefaultLabelSpec);

}  // namespace lugbulk::labels_pdf

// Renders LabelRecords onto an Avery 5160 label sheet PDF — port of
// lugbulk-label's render_labels.py. Layout:
//     [thumb]  Element ID: 1234567      Qty: 150
//              Color
//              PART NAME
//                 Person Name (bold, larger, centered)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sheet_pivot.h"

namespace lugbulk::labels_pdf {

// `image_cache_dir` is where part thumbnails are cached across runs/sheets
// (keyed by element id, shared across all sheets — LEGO element photos
// aren't sheet- or user-specific). A cached miss (404/timeout/etc.) is
// stored as an empty file and retried after 24h, same as the CLI, so a
// transient CDN outage doesn't permanently blank out a thumbnail.
//
// Returns the built PDF as an in-memory buffer — nothing is written to
// disk except the (non-sensitive, shared) image cache.
std::vector<uint8_t> build_labels_pdf(const std::vector<LabelRecord>& records,
                                       const std::string& image_cache_dir);

}  // namespace lugbulk::labels_pdf

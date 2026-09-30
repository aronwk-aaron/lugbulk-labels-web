// Strict JSON checking, for JSON the server stores as sent and hands back
// verbatim (a sheet's report options: see PUT /sheets/:id/design). Crow's
// parser is lenient and can't give back a member's original text; this
// follows RFC 8259 exactly (and requires valid UTF-8), so anything it
// accepts is JSON every browser's JSON.parse reads.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace lugbulk::json_check {

// The most nesting accepted (objects and arrays).
constexpr int kMaxDepth = 32;

// The largest report_options object accepted, in bytes.
constexpr size_t kMaxReportOptionsBytes = 4096;

// Whether `text` is exactly one JSON value (surrounding whitespace allowed).
bool valid(std::string_view text);

// For a JSON object `text`: the original text of its top-level member
// `key` (compared as written, without unescaping), or "" when there's no
// such member. nullopt if `text` isn't a valid JSON object or has `key`
// more than once.
std::optional<std::string_view> member(std::string_view text, std::string_view key);

// Why `raw` can't be stored as a sheet's report options (over
// kMaxReportOptionsBytes, or not a JSON object), or nullopt if it can.
std::optional<std::string> report_options_error(std::string_view raw);

}  // namespace lugbulk::json_check

// PoDoFo's Base14 fonts (Helvetica etc.) default to WinAnsiEncoding
// (Windows-1252) — the plain `PdfString(const char*)` constructor expects
// bytes already in that single-byte encoding, not UTF-8. All our own text
// (sheet cell values from Google, hand-written labels) arrives as UTF-8,
// so every string destined for a PdfString/DrawText call must go through
// this conversion first, or accented/typographic characters render as
// mojibake (or crash on characters WinAnsi can't represent).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace lugbulk::pdf_text {

// Transcodes UTF-8 to Windows-1252 (WinAnsiEncoding's charset). Characters
// outside CP1252 (most non-Latin scripts, many symbols/emoji) are replaced
// with '?' rather than throwing — label/report text should never fail to
// render outright over an unsupported glyph.
std::string to_winansi(const std::string& utf8);

// Breaks `text` into lines no wider than `max_width`, as measured by
// `width` (of a line's bytes; WinAnsi text is one byte per character).
// Lines break at spaces; a single word wider than a line is split between
// characters as a last resort. Nothing is dropped but the spaces a line
// breaks at. Always at least one line (empty text gives {""}).
std::vector<std::string> wrap_lines(const std::string& text, double max_width,
                                    const std::function<double(const std::string&)>& width);

}  // namespace lugbulk::pdf_text

// PoDoFo's Base14 fonts (Helvetica etc.) default to WinAnsiEncoding
// (Windows-1252) — the plain `PdfString(const char*)` constructor expects
// bytes already in that single-byte encoding, not UTF-8. All our own text
// (sheet cell values from Google, hand-written labels) arrives as UTF-8,
// so every string destined for a PdfString/DrawText call must go through
// this conversion first, or accented/typographic characters render as
// mojibake (or crash on characters WinAnsi can't represent).
#pragma once

#include <string>

namespace lugbulk::pdf_text {

// Transcodes UTF-8 to Windows-1252 (WinAnsiEncoding's charset). Characters
// outside CP1252 (most non-Latin scripts, many symbols/emoji) are replaced
// with '?' rather than throwing — label/report text should never fail to
// render outright over an unsupported glyph.
std::string to_winansi(const std::string& utf8);

}  // namespace lugbulk::pdf_text

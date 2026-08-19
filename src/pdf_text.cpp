#include "pdf_text.h"

#include <cstdint>

namespace lugbulk::pdf_text {

namespace {

// NOTE: this table targets PoDoFo 0.9.8's *actual* WinAnsiEncoding glyph
// assignment in the 0x80-0x9F range, empirically verified by rendering
// every byte in that range and reading back the glyph — it is NOT
// standard Windows-1252 (PoDoFo's table is shifted/reordered from the real
// thing, e.g. real CP1252 em-dash 0x97 renders as PoDoFo's 'Š'; PoDoFo's
// em-dash is 0x84). Do not "fix" this to match the CP1252 spec without
// re-verifying against PoDoFo's rendering — see labels_pdf tests.
uint32_t cp1252_high_range_from_codepoint(uint32_t codepoint, bool* found) {
    *found = true;
    switch (codepoint) {
        case 0x2022: return 0x80;  // • (bullet)
        case 0x2020: return 0x81;  // †
        case 0x2021: return 0x82;  // ‡
        case 0x2026: return 0x83;  // …
        case 0x2014: return 0x84;  // — (em dash)
        case 0x2013: return 0x85;  // – (en dash)
        case 0x0192: return 0x86;  // ƒ
        case 0x2039: return 0x88;  // ‹
        case 0x203A: return 0x89;  // ›
        case 0x2030: return 0x8B;  // ‰
        case 0x201E: return 0x8C;  // „
        case 0x201C: return 0x8D;  // "
        case 0x201D: return 0x8E;  // "
        case 0x2018: return 0x8F;  // '
        case 0x2019: return 0x90;  // '
        case 0x201A: return 0x91;  // ‚
        case 0x2122: return 0x92;  // ™
        case 0x0152: return 0x96;  // Œ
        case 0x0160: return 0x97;  // Š
        case 0x0178: return 0x98;  // Ÿ
        case 0x017D: return 0x99;  // Ž
        case 0x0153: return 0x9C;  // œ
        case 0x0161: return 0x9D;  // š
        case 0x017E: return 0x9E;  // ž
        default: *found = false; return '?';
    }
}

// Decodes one UTF-8 codepoint starting at `i`; advances `i` past it.
// Malformed sequences consume one byte and yield U+FFFD, so garbage input
// can't desync into reading nonsense follow bytes as separate codepoints.
uint32_t decode_utf8(const std::string& s, size_t& i) {
    unsigned char c0 = static_cast<unsigned char>(s[i]);
    auto continuation = [&](size_t idx) {
        return idx < s.size() && (static_cast<unsigned char>(s[idx]) & 0xC0) == 0x80;
    };

    if (c0 < 0x80) {
        i += 1;
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && continuation(i + 1)) {
        uint32_t cp = (c0 & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && continuation(i + 1) && continuation(i + 2)) {
        uint32_t cp = (c0 & 0x0Fu) << 12 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6 |
                      (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
        return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && continuation(i + 1) && continuation(i + 2) && continuation(i + 3)) {
        uint32_t cp = (c0 & 0x07u) << 18 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12 |
                      (static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6 |
                      (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
        return cp;
    }
    i += 1;
    return 0xFFFD;
}

}  // namespace

std::string to_winansi(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());

    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = decode_utf8(utf8, i);

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp >= 0xA0 && cp <= 0xFF) {
            // Latin-1 supplement maps 1:1 onto CP1252 in this range.
            out.push_back(static_cast<char>(cp));
        } else if (cp == 0xA0) {
            out.push_back(static_cast<char>(0xA0));  // nbsp
        } else {
            bool found = false;
            uint32_t byte = cp1252_high_range_from_codepoint(cp, &found);
            out.push_back(found ? static_cast<char>(byte) : '?');
        }
    }
    return out;
}

}  // namespace lugbulk::pdf_text

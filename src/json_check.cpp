#include "json_check.h"

#include <cstdint>

namespace lugbulk::json_check {

namespace {

// A recursive-descent checker over `s`. On a top-level object it can note
// where one member's value starts and ends.
class Checker {
public:
    Checker(std::string_view s, std::string_view key) : s_(s), key_(key) {}

    bool document() {
        ws();
        if (!value(0)) return false;
        ws();
        return pos_ == s_.size();
    }

    bool top_is_object() const { return top_object_; }
    int key_count() const { return key_count_; }
    std::string_view key_value() const { return key_value_; }

private:
    bool at_end() const { return pos_ >= s_.size(); }
    unsigned char peek() const { return static_cast<unsigned char>(s_[pos_]); }

    void ws() {
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++pos_;
        }
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(int depth) {
        if (at_end()) return false;
        switch (peek()) {
            case '{': return object(depth + 1);
            case '[': return array(depth + 1);
            case '"': return string();
            case 't': return literal("true");
            case 'f': return literal("false");
            case 'n': return literal("null");
            default: return number();
        }
    }

    bool object(int depth) {
        if (depth > kMaxDepth) return false;
        const bool top = depth == 1;
        if (top) top_object_ = true;
        ++pos_;  // '{'
        ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            if (at_end() || peek() != '"') return false;
            size_t key_start = pos_ + 1;
            if (!string()) return false;
            std::string_view key = s_.substr(key_start, pos_ - 1 - key_start);
            ws();
            if (at_end() || peek() != ':') return false;
            ++pos_;
            ws();
            size_t value_start = pos_;
            if (!value(depth)) return false;
            if (top && key == key_) {
                ++key_count_;
                key_value_ = s_.substr(value_start, pos_ - value_start);
            }
            ws();
            if (at_end()) return false;
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            if (peek() != ',') return false;
            ++pos_;
        }
    }

    bool array(int depth) {
        if (depth > kMaxDepth) return false;
        ++pos_;  // '['
        ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            if (!value(depth)) return false;
            ws();
            if (at_end()) return false;
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            if (peek() != ',') return false;
            ++pos_;
        }
    }

    static bool hex(unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // One UTF-8 sequence starting at pos_ (a byte >= 0x80): no overlong
    // forms, no surrogates, nothing past U+10FFFF.
    bool utf8() {
        unsigned char c = peek();
        int extra;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
        else return false;
        if (pos_ + extra >= s_.size()) return false;
        for (int i = 1; i <= extra; ++i) {
            unsigned char cc = static_cast<unsigned char>(s_[pos_ + i]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        pos_ += extra + 1;
        return true;
    }

    bool string() {
        ++pos_;  // opening quote
        while (!at_end()) {
            unsigned char c = peek();
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return false;
            if (c == '\\') {
                ++pos_;
                if (at_end()) return false;
                unsigned char e = peek();
                if (e == 'u') {
                    if (pos_ + 4 >= s_.size()) return false;
                    for (int i = 1; i <= 4; ++i) {
                        if (!hex(static_cast<unsigned char>(s_[pos_ + i]))) return false;
                    }
                    pos_ += 5;
                } else if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                           e == 'r' || e == 't') {
                    ++pos_;
                } else {
                    return false;
                }
            } else if (c >= 0x80) {
                if (!utf8()) return false;
            } else {
                ++pos_;
            }
        }
        return false;  // unterminated
    }

    bool digits() {
        size_t start = pos_;
        while (!at_end() && peek() >= '0' && peek() <= '9') ++pos_;
        return pos_ > start;
    }

    bool number() {
        if (!at_end() && peek() == '-') ++pos_;
        if (at_end()) return false;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            digits();
        } else {
            return false;
        }
        if (!at_end() && peek() == '.') {
            ++pos_;
            if (!digits()) return false;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) ++pos_;
            if (!digits()) return false;
        }
        return true;
    }

    std::string_view s_;
    std::string_view key_;
    size_t pos_ = 0;
    bool top_object_ = false;
    int key_count_ = 0;
    std::string_view key_value_;
};

}  // namespace

bool valid(std::string_view text) {
    Checker c(text, {});
    return c.document();
}

std::optional<std::string_view> member(std::string_view text, std::string_view key) {
    Checker c(text, key);
    if (!c.document() || !c.top_is_object() || c.key_count() > 1) return std::nullopt;
    return c.key_count() == 1 ? c.key_value() : std::string_view();
}

std::optional<std::string> report_options_error(std::string_view raw) {
    if (raw.size() > kMaxReportOptionsBytes) return std::string("report_options is over 4 KB");
    Checker c(raw, {});
    if (!c.document() || !c.top_is_object()) {
        return std::string("report_options must be a JSON object");
    }
    return std::nullopt;
}

}  // namespace lugbulk::json_check

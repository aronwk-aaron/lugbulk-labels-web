// Which parts of a label are drawn: the design switches saved with a sheet
// (the labels themselves are laid out and drawn in the browser,
// static/js/labels.js, which has the same part names). Here only to check
// and normalize a saved design.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace lugbulk::labels {

// Parts of a label that can be switched on and off.
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

// "Keep each part on one sheet": "off" or "optimize" (static/js/packing.js).
bool keep_parts_valid(std::string_view value);

}  // namespace lugbulk::labels

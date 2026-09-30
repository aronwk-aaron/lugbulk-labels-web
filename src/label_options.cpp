#include "label_options.h"

namespace lugbulk::labels {

std::optional<LabelOptions> LabelOptions::parse(const std::string& hide, const std::string& show,
                                                std::string* error) {
    LabelOptions opts;
    for (const auto& [list, hidden] : {std::pair{&hide, true}, std::pair{&show, false}}) {
        size_t start = 0;
        while (start <= list->size()) {
            size_t end = list->find(',', start);
            if (end == std::string::npos) end = list->size();
            std::string name = list->substr(start, end - start);
            name.erase(0, name.find_first_not_of(' '));
            name.erase(name.find_last_not_of(' ') + 1);
            if (!name.empty()) {
                auto part = label_part_from_name(name);
                if (!part) {
                    if (error) *error = "unknown label part '" + name + "'";
                    return std::nullopt;
                }
                opts.set(*part, !hidden);
            }
            start = end + 1;
        }
    }
    return opts;
}

std::optional<LabelOptions> LabelOptions::from_hidden(const std::string& hidden,
                                                      std::string* error) {
    // Show everything (including the QR code), then hide what's listed.
    auto opts = parse(hidden, "", error);
    if (!opts) return std::nullopt;
    bool qr_listed = false;
    size_t start = 0;
    while (start <= hidden.size()) {
        size_t end = hidden.find(',', start);
        if (end == std::string::npos) end = hidden.size();
        std::string name = hidden.substr(start, end - start);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        qr_listed |= name == "qr";
        start = end + 1;
    }
    opts->set(LabelPart::kQr, !qr_listed);
    return opts;
}

std::string LabelOptions::hidden_csv() const {
    std::string out;
    for (size_t i = 0; i < kLabelPartNames.size(); ++i) {
        if (!show(static_cast<LabelPart>(i))) out += (out.empty() ? "" : ",") + std::string(kLabelPartNames[i]);
    }
    return out;
}

std::optional<LabelPart> label_part_from_name(std::string_view name) {
    for (size_t i = 0; i < kLabelPartNames.size(); ++i) {
        if (kLabelPartNames[i] == name) return static_cast<LabelPart>(i);
    }
    return std::nullopt;
}

}  // namespace lugbulk::labels

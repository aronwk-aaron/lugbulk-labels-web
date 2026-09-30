#include "ordering.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>

namespace lugbulk::ordering {

namespace {

// Rough mass of one 1x1x1 brick-volume of ABS, in grams. Only used to put
// description-derived sizes on the same scale as real per-piece weights.
constexpr double kGramsPerBrickUnit = 0.43;

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// How people sort: by last name (the last whitespace-separated word,
// lower-cased), then by the whole name — static/js/ordering.js personSortKey.
std::pair<std::string, std::string> person_sort_key(const std::string& person) {
    std::istringstream iss(person);
    std::string token, last;
    while (iss >> token) last = token;
    return {last.empty() ? to_lower(person) : to_lower(last), to_lower(person)};
}

}  // namespace

std::optional<PartOrder> parse_part_order(const std::string& s) {
    if (s == "heaviest") return PartOrder::kHeaviest;
    if (s == "lightest") return PartOrder::kLightest;
    if (s == "sheet") return PartOrder::kSheet;
    return std::nullopt;
}

std::optional<double> estimate_weight(const std::string& description) {
    // footprint in studs, then an optional height in bricks: "1X2X5",
    // "1X1X1 2/3", "2X2X2/3" (2/3 of a brick), or "1X2X3/73°" (3 bricks
    // tall, 73° slope — not 3/73).
    static const std::regex kDims(
        R"((\d+)\s*X\s*(\d+)(?:\s*X\s*(\d+)(?:\s+(\d+)/(\d+)|/(\d+)(°)?)?)?)");
    static const std::regex kFullHeight(R"(\b(BRICK|ROOF|DUPLO)\b)");
    static const std::regex kThirdHeight(R"(\b(PLATE|TILE|PLADE)\b)");

    std::string desc = to_upper(description);
    std::smatch m;
    if (!std::regex_search(desc, m, kDims)) return std::nullopt;

    double height = 1.0;
    if (m[3].matched) {
        height = std::stod(m[3].str());
        if (m[4].matched) {
            height += std::stod(m[4].str()) / std::stod(m[5].str());
        } else if (m[6].matched && !m[7].matched && m[6].str() == "3") {
            height /= 3;
        }
    } else if (!std::regex_search(desc, kFullHeight) && std::regex_search(desc, kThirdHeight)) {
        height = 1.0 / 3;  // plates and tiles are a third of a brick tall
    }

    double volume = std::stod(m[1].str()) * std::stod(m[2].str()) * height;
    if (desc.find("DUPLO") != std::string::npos) volume *= 8;  // twice the size every way
    return volume * kGramsPerBrickUnit;
}

std::vector<PartSummary> summarize_parts(const std::vector<LabelRecord>& records,
                                         PartOrder order) {
    std::vector<PartSummary> parts;  // sheet order
    std::map<std::string, size_t> index;
    for (const auto& r : records) {
        auto [it, inserted] = index.emplace(r.element_id, parts.size());
        if (inserted) {
            PartSummary p;
            p.element_id = r.element_id;
            p.description = r.description;
            p.lego_color = r.lego_color;
            p.bl_color = r.bl_color;
            if (r.weight) {
                p.weight = r.weight;
                p.weight_source = "sheet";
            } else if (r.catalog_weight) {
                p.weight = r.catalog_weight;
                p.weight_source = "bricklink";
            } else if ((p.weight = estimate_weight(r.description))) {
                p.weight_source = "estimate";
            }
            parts.push_back(std::move(p));
        }
        PartSummary& p = parts[it->second];
        p.lots += 1;
        p.pieces += parse_qty(r.qty);
    }

    if (order == PartOrder::kSheet) return parts;
    double sign = order == PartOrder::kHeaviest ? -1 : 1;
    // Stable: unknown-weight parts keep sheet order after the rest.
    std::stable_sort(parts.begin(), parts.end(), [&](const PartSummary& a, const PartSummary& b) {
        if (a.weight.has_value() != b.weight.has_value()) return a.weight.has_value();
        if (!a.weight) return false;
        return sign * *a.weight < sign * *b.weight;
    });
    return parts;
}

std::vector<LabelRecord> order_records(std::vector<LabelRecord> records, PartOrder order) {
    std::map<std::string, std::vector<LabelRecord>> by_part;
    std::vector<PartSummary> parts = summarize_parts(records, order);
    for (auto& r : records) by_part[r.element_id].push_back(std::move(r));

    std::vector<LabelRecord> ordered;
    ordered.reserve(records.size());
    for (const auto& part : parts) {
        auto& group = by_part[part.element_id];
        std::stable_sort(group.begin(), group.end(), [](const LabelRecord& a, const LabelRecord& b) {
            double qa = parse_qty(a.qty), qb = parse_qty(b.qty);
            if (qa != qb) return qa < qb;
            // Same tie-break as the lot-count report: last name, then full name.
            return person_sort_key(a.person) < person_sort_key(b.person);
        });
        int total = static_cast<int>(group.size());
        for (int i = 0; i < total; ++i) {
            group[i].part_seq = i + 1;
            group[i].part_total = total;
            ordered.push_back(std::move(group[i]));
        }
    }
    return ordered;
}

}  // namespace lugbulk::ordering

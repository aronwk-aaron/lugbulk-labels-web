#include "records.h"

#include <set>

#include "colors.h"
#include "crow/json.h"

namespace lugbulk::records {

void cap_rows(std::vector<std::vector<std::string>>& rows) {
    if (rows.size() > static_cast<size_t>(kMaxSheetRows)) rows.resize(kMaxSheetRows);
}

void check_run_size(const PivotResult& pivot) {
    std::set<std::string> parts;
    for (const auto& r : pivot.records) parts.insert(r.element_id);
    if (pivot.records.size() > kMaxLabels || parts.size() > kMaxParts) {
        throw TooBig("this sheet has " + std::to_string(pivot.records.size()) + " labels and " +
                     std::to_string(parts.size()) + " parts; the limit is " +
                     std::to_string(kMaxLabels) + " labels / " + std::to_string(kMaxParts) +
                     " parts per run");
    }
}

PivotResult pivot_tabs(std::vector<std::vector<std::vector<std::string>>> tabs) {
    PivotResult pivot;
    for (auto& rows : tabs) {
        cap_rows(rows);
        pivot = pivot_sheet(rows);
        if (!pivot.records.empty()) break;
    }
    check_run_size(pivot);
    return pivot;
}

void apply_bricklink(PivotResult& pivot, const bricklink::Catalog& catalog) {
    if (catalog.empty() || pivot.records.empty()) return;
    std::set<std::string> colored;
    for (auto& r : pivot.records) {
        auto it = catalog.find(r.element_id);
        if (it == catalog.end()) continue;
        r.catalog_weight = it->second.weight;
        if (r.bl_color.empty() && !it->second.color.empty()) {
            r.bl_color = it->second.color;
            if (r.lego_color.empty()) r.lego_color = colors::resolve("", r.bl_color).lego;
            colored.insert(r.element_id);
        }
    }
    // A color BrickLink supplied is no longer missing.
    std::erase_if(pivot.issues, [&](const SheetIssue& i) {
        return i.kind == "missing_color" && colored.count(i.element_id);
    });
}

std::string check_summary_json(const PivotResult& pivot) {
    crow::json::wvalue body;
    body["labels"] = pivot.records.size();
    std::set<std::string> people, parts;
    for (const auto& r : pivot.records) {
        people.insert(r.person);
        parts.insert(r.element_id);
    }
    body["people"] = people.size();
    body["parts"] = parts.size();
    std::vector<crow::json::wvalue> issues;
    for (const auto& issue : pivot.issues) {
        crow::json::wvalue item;
        item["row"] = issue.row;
        item["kind"] = issue.kind;
        item["detail"] = issue.detail;
        issues.push_back(std::move(item));
    }
    body["issues"] = std::move(issues);
    return body.dump();
}

}  // namespace lugbulk::records

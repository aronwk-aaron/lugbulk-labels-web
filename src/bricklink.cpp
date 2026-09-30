#include "bricklink.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>

#include "crow/json.h"
#include "sheet_pivot.h"

namespace lugbulk::bricklink {

namespace {

std::vector<std::string> split_tabs(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return out;
        start = tab + 1;
    }
}

std::string lower_trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::optional<size_t> column(const std::vector<std::string>& header, const std::string& name,
                             bool prefix = false) {
    for (size_t i = 0; i < header.size(); ++i) {
        std::string h = lower_trim(header[i]);
        if (prefix ? h.rfind(name, 0) == 0 : h == name) return i;
    }
    return std::nullopt;
}

std::optional<double> parse_weight(const std::string& text) {
    try {
        size_t used = 0;
        double w = std::stod(text, &used);
        if (used > 0 && w > 0) return w;
    } catch (const std::exception&) {
    }
    return std::nullopt;  // "?" = unknown
}

std::vector<std::string> files_in(const std::string& folder) {
    std::vector<std::string> out;
    if (DIR* dir = opendir(folder.c_str())) {
        while (dirent* e = readdir(dir)) {
            std::string path = folder + "/" + e->d_name;
            struct stat st{};
            if (e->d_name[0] != '.' && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                out.push_back(path);
            }
        }
        closedir(dir);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

Catalog load(const std::string& folder) {
    std::optional<std::map<std::string, std::optional<double>>> weights;
    std::optional<std::map<std::string, std::pair<std::string, std::string>>> codes;

    for (const std::string& path : files_in(folder)) {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        if (!std::getline(in, line)) continue;
        auto header = split_tabs(line);
        auto number = column(header, "number"), weight = column(header, "weight", true);
        auto item = column(header, "item no"), color = column(header, "color"),
             code = column(header, "code");
        if (number && weight) {
            weights.emplace();
            while (std::getline(in, line)) {
                auto r = split_tabs(line);
                if (r.size() > std::max(*number, *weight)) (*weights)[r[*number]] = parse_weight(r[*weight]);
            }
        } else if (item && color && code) {
            codes.emplace();
            while (std::getline(in, line)) {
                auto r = split_tabs(line);
                if (r.size() > std::max({*item, *color, *code})) {
                    codes->emplace(lower_trim(r[*code]), std::make_pair(r[*item], r[*color]));
                }
            }
        }
    }

    Catalog catalog;
    if (!weights || !codes) return catalog;
    for (const auto& [element, part_color] : *codes) {
        auto w = weights->find(part_color.first);
        catalog.emplace(element, PartInfo{part_color.first, part_color.second,
                                          w == weights->end() ? std::nullopt : w->second});
    }
    return catalog;
}

std::shared_ptr<const Catalog> CatalogCache::get() {
    std::string signature;
    for (const std::string& path : files_in(folder_)) {
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) {
            signature += path + ":" + std::to_string(st.st_mtime) + ":" + std::to_string(st.st_size) + ";";
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (signature != signature_) {
        catalog_ = std::make_shared<const Catalog>(load(folder_));
        signature_ = signature;
        if (catalog_->empty()) {
            std::cerr << "bricklink: no catalog in " << folder_
                      << " (needs the Parts-with-weight and Part and Color Codes downloads);"
                         " weights will be estimated" << std::endl;
        } else {
            std::cerr << "bricklink: loaded " << catalog_->size() << " element codes from "
                      << folder_ << std::endl;
        }
    }
    return catalog_;
}

std::optional<std::vector<std::string>> parse_lookup_request(const std::string& body,
                                                             std::string* error) {
    auto json = crow::json::load(body);
    if (!json || json.t() != crow::json::type::Object || !json.has("ids") ||
        json["ids"].t() != crow::json::type::List) {
        *error = R"(expected {"ids":["6225242", ...]})";
        return std::nullopt;
    }
    if (json["ids"].size() > kMaxLookupIds) {
        *error = "at most " + std::to_string(kMaxLookupIds) + " ids per lookup";
        return std::nullopt;
    }
    std::vector<std::string> ids;
    for (const auto& v : json["ids"]) {
        if (v.t() != crow::json::type::String || !is_valid_element_id(std::string(v.s()))) {
            *error = "every id must be a LEGO element ID (4-8 digits)";
            return std::nullopt;
        }
        ids.push_back(std::string(v.s()));
    }
    return ids;
}

std::string lookup_json(const Catalog& catalog, const std::vector<std::string>& ids) {
    std::string out = "{";
    std::set<std::string> seen;
    for (const auto& id : ids) {
        auto it = catalog.find(id);
        if (it == catalog.end() || !seen.insert(id).second) continue;
        std::string key, part, color;
        crow::json::escape(id, key);
        crow::json::escape(it->second.part_no, part);
        crow::json::escape(it->second.color, color);
        std::string weight = "null";
        if (it->second.weight && std::isfinite(*it->second.weight)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.10g", *it->second.weight);
            weight = buf;
        }
        if (out.size() > 1) out += ",";
        out += "\"" + key + "\":{\"part\":\"" + part + "\",\"color\":\"" + color +
               "\",\"weight\":" + weight + "}";
    }
    return out + "}";
}

}  // namespace lugbulk::bricklink

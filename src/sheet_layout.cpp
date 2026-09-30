#include "sheet_layout.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "crow/json.h"

namespace lugbulk::layout {

namespace {

std::vector<LabelSpec> g_specs;
std::map<std::string, size_t> g_aliases;  // normalized name -> index

std::string normalize(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string inches(double mm) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f\"", mm / 25.4);
    return buf;
}

}  // namespace

std::string LabelSpec::display_name() const {
    std::string size = inches(label_height_mm) + " x " + inches(label_width_mm);
    std::string count = page == "roll" ? "roll" : std::to_string(per_sheet()) + "/sheet";
    return brand + " " + part + " \xe2\x80\x94 " + description + ", " + size + ", " + count;
}

void load_label_specs(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("label specs: can't open " + path);
    std::stringstream buf;
    buf << in.rdbuf();
    auto json = crow::json::load(buf.str());
    if (!json || !json.has("specs")) throw std::runtime_error("label specs: malformed " + path);

    std::vector<LabelSpec> specs;
    for (const auto& j : json["specs"]) {
        LabelSpec s;
        s.id = std::string(j["id"].s());
        s.brand = std::string(j["brand"].s());
        s.part = std::string(j["part"].s());
        s.description = std::string(j["description"].s());
        s.page = std::string(j["page"].s());
        for (const auto& e : j["equivalents"]) s.equivalents.push_back(std::string(e.s()));
        s.sheet_width_mm = j["sheet_width_mm"].d();
        s.sheet_height_mm = j["sheet_height_mm"].d();
        s.columns = static_cast<int>(j["columns"].i());
        s.rows = static_cast<int>(j["rows"].i());
        s.label_width_mm = j["label_width_mm"].d();
        s.label_height_mm = j["label_height_mm"].d();
        s.left_margin_mm = j["left_margin_mm"].d();
        s.top_margin_mm = j["top_margin_mm"].d();
        s.row_gap_mm = j["row_gap_mm"].d();
        s.column_gap_mm = j["column_gap_mm"].d();
        if (s.columns < 1 || s.rows < 1 || s.label_width_mm <= 0 || s.label_height_mm <= 0) {
            throw std::runtime_error("label specs: bad geometry for " + s.id);
        }
        specs.push_back(std::move(s));
    }

    std::map<std::string, size_t> aliases;
    for (size_t i = 0; i < specs.size(); ++i) aliases.emplace(specs[i].id, i);
    for (size_t i = 0; i < specs.size(); ++i) {
        std::vector<std::string> parts = specs[i].equivalents;
        parts.push_back(specs[i].part);
        for (const auto& part : parts) {
            aliases.emplace(normalize(specs[i].brand + part), i);
            aliases.emplace(normalize(part), i);  // bare part number
        }
    }
    g_specs = std::move(specs);
    g_aliases = std::move(aliases);
    if (!find_label_spec(kDefaultLabelSpecId)) {
        throw std::runtime_error(std::string("label specs: default stock ") + kDefaultLabelSpecId +
                                 " missing from " + path);
    }
}

const std::vector<LabelSpec>& label_specs() { return g_specs; }

const LabelSpec* find_label_spec(std::string_view name) {
    auto it = g_aliases.find(normalize(name));
    return it == g_aliases.end() ? nullptr : &g_specs[it->second];
}

const LabelSpec& default_label_spec() { return *find_label_spec(kDefaultLabelSpecId); }

}  // namespace lugbulk::layout

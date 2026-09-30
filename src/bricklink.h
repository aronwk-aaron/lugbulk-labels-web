// Part weights and BrickLink color names from BrickLink's catalog download
// — port of lugbulk-label's bricklink.py.
//
// BrickLink's API is for sellers only, but any free member can download the
// catalog (https://www.bricklink.com/catalogDownload.asp) as Tab-Delimited
// files. Two are read from a folder ($LUGBULK_DATA_DIR/bricklink/):
//   - Catalog Items -> Parts, with "Include Weight": BrickLink part number
//     -> grams (header "Number" ... "Weight (in Grams)"; "?" = unknown);
//   - Part and Color Codes: LEGO element ID -> BrickLink part and color
//     (header "Item No", "Color", "Code").
// Files are recognised by their header row, not their names, and reloaded
// when they change, so updating the data is just replacing the files.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lugbulk::bricklink {

struct PartInfo {
    std::string part_no;           // BrickLink item number, e.g. "3004"
    std::string color;             // BrickLink color name
    std::optional<double> weight;  // grams per piece; nullopt if BrickLink doesn't know
};

using Catalog = std::map<std::string, PartInfo>;  // LEGO element ID -> info

// Reads the catalog files in `folder`. Empty if the folder or either file
// is missing.
Catalog load(const std::string& folder);

// The catalog in `folder`, reloaded only when its files change (checked by
// modification time and size on each call — a few stat()s). Thread-safe.
class CatalogCache {
public:
    explicit CatalogCache(std::string folder) : folder_(std::move(folder)) {}
    std::shared_ptr<const Catalog> get();

private:
    std::string folder_;
    std::mutex mu_;
    std::string signature_;
    std::shared_ptr<const Catalog> catalog_ = std::make_shared<Catalog>();
};

}  // namespace lugbulk::bricklink
